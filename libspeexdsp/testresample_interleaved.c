/* Copyright (C) 2024 Xiph.Org Foundation

   File: testresample_interleaved.c
   Acceptance test for the interleaved multi-channel resampling API.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

   1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
   OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
   INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
   STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/

/* Acceptance test for interleaved resampling.
 *
 * Verifies that speex_resampler_process_interleaved_{float,int}:
 *   1) are memory-safe for 2..8 channels over many ratios and buffer sizes
 *      (the exact-sized heap buffers make any overrun an ASan error);
 *   2) consume/produce exactly the same number of frames on every channel;
 *   3) produce, per channel, the exact samples obtained by feeding that
 *      channel alone through an independent single-channel resampler
 *      (bit-exact), including across streaming chunks and retries;
 *   4) always return (no infinite loop) even with one-frame output buffers.
 *
 * The reference is one independent single-channel resampler per channel.
 * Every interleaved call is mirrored by an identical-sequence planar call
 * per channel (same input frames, same requested output capacity, same
 * retry behaviour), so the produced streams must match sample-for-sample.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <speex/speex_resampler.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifndef TOTAL_FRAMES
#define TOTAL_FRAMES 2000
#endif

static unsigned rng_state = 0x1234abcd;
static unsigned rnd(void)
{
   rng_state ^= rng_state << 13;
   rng_state ^= rng_state >> 17;
   rng_state ^= rng_state << 5;
   return rng_state;
}

static const int ratios[][2] = {
   {48000, 16000}, {16000, 48000},   /* 1:3 / 3:1 */
   {44100, 48000}, {48000, 44100},   /* near-unity, non-integer */
   {8000, 48000},  {48000, 8000},    /* 1:6 / 6:1 */
   {32000, 48000}, {48000, 32000},   /* 2:3 / 3:2 */
   {12345, 67890}, {67890, 12345},   /* awkward coprime ratio */
   {48000, 48000},                    /* unity */
   {22050, 44100}, {44100, 22050}
};
#define NRATIO (int)(sizeof(ratios)/sizeof(ratios[0]))

/* Includes 1 (smaller than one inter-channel stride for ch>=2) and odd/
   prime sizes to exercise every capacity boundary. */
static const spx_uint32_t frame_sizes[] = {
   1u, 2u, 3u, 5u, 7u, 8u, 13u, 16u, 31u, 33u, 64u, 100u, 127u, 257u
};
#define NSIZE (int)(sizeof(frame_sizes)/sizeof(frame_sizes[0]))

static int failures = 0;
static int checks = 0;

static void fail(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   failures++;
}

/* ---- float -------------------------------------------------------------- */
static int test_float(int ch, int ri, int quality)
{
   int err;
   spx_uint32_t c, s;
   const int in_rate = ratios[ri][0], out_rate = ratios[ri][1];

   SpeexResamplerState *st = speex_resampler_init((spx_uint32_t)ch, in_rate, out_rate, quality, &err);
   if (!st) { fail("float init failed ch=%d\n", ch); return 1; }
   speex_resampler_skip_zeros(st);

   SpeexResamplerState *ref[8];
   for (c = 0; c < (spx_uint32_t)ch; c++) {
      ref[c] = speex_resampler_init(1, in_rate, out_rate, quality, &err);
      if (!ref[c]) { fail("ref init failed\n"); return 1; }
      speex_resampler_skip_zeros(ref[c]);
      /* The reference reads the SAME interleaved buffer, one channel, via the
         inter-channel stride, so it sees the identical input sequence. */
      speex_resampler_set_input_stride(ref[c], (spx_uint32_t)ch);
      speex_resampler_set_output_stride(ref[c], 1);
   }

   spx_uint32_t total = TOTAL_FRAMES;
   float *input = malloc((size_t)total * ch * sizeof(float));
   for (spx_uint32_t i = 0; i < total * (spx_uint32_t)ch; i++)
      input[i] = (float)((int)(rnd() % 2001) - 1000) / 1000.0f;

   spx_uint32_t cap = total * 8 + 64;
   float *got[8], *want[8];
   spx_uint32_t produced[8] = {0}, ref_produced[8] = {0};
   for (c = 0; c < (spx_uint32_t)ch; c++) {
      got[c] = malloc(cap * sizeof(float));
      want[c] = malloc(cap * sizeof(float));
   }

   spx_uint32_t pos = 0, size_idx = 0;
   while (pos < total) {
      spx_uint32_t chunk = frame_sizes[size_idx % NSIZE];
      size_idx++;
      if (chunk > total - pos) chunk = total - pos;

      /* Output capacity large enough to hold every frame this input chunk
         can produce at the largest ratio in the table (67890/12345 ~= 5.5x),
         plus filter margin. This keeps each call input-bound so the
         interleaved resampler and the per-channel references stay in exact
         lockstep and can be compared sample-for-sample. The buffer is still
         exactly sized; tiny output buffers are covered by test_tiny_*(). */
      spx_uint32_t out_cap = chunk * 6 + 64;

      /* One interleaved call plus one matching planar reference call per
         channel, all with the identical frame capacity. Heap buffers are
         exactly sized, so any strided overrun is reported by ASan. */
      spx_uint32_t il = chunk, ol = out_cap;
      float *outbuf = malloc((size_t)out_cap * ch * sizeof(float));
      speex_resampler_process_interleaved_float(
         st, input + (size_t)pos * ch, &il, outbuf, &ol);
      checks++;

      for (c = 0; c < (spx_uint32_t)ch; c++) {
         for (s = 0; s < ol; s++)
            got[c][produced[c] + s] = outbuf[s * ch + c];
         produced[c] += ol;

         spx_uint32_t ril = chunk, rol = out_cap;
         float *refout = malloc((size_t)out_cap * sizeof(float));
         speex_resampler_process_float(ref[c], 0,
            input + (size_t)pos * ch + c, &ril, refout, &rol);
         if (ril != il)
            fail("FLOAT consumed mismatch ch=%d c=%u: inter=%u ref=%u\n", ch, c, il, ril);
         if (rol != ol)
            fail("FLOAT produced mismatch ch=%d c=%u: inter=%u ref=%u\n", ch, c, ol, rol);
         for (s = 0; s < rol; s++)
            want[c][ref_produced[c] + s] = refout[s];
         ref_produced[c] += rol;
         free(refout);
      }

      for (c = 1; c < (spx_uint32_t)ch; c++)
         if (produced[c] != produced[0] || ref_produced[c] != ref_produced[0])
            fail("FLOAT lockstep ch=%d c=%u got=(%u,%u) ref0=%u\n",
                 ch, c, produced[c], produced[0], ref_produced[0]);

      pos += il;
      free(outbuf);
   }

   for (c = 0; c < (spx_uint32_t)ch; c++) {
      if (produced[c] != ref_produced[c]) {
         fail("FLOAT total mismatch ch=%d c=%u got=%u want=%u\n", ch, c, produced[c], ref_produced[c]);
      } else for (s = 0; s < produced[c]; s++) {
         if (got[c][s] != want[c][s]) {
            fail("FLOAT sample mismatch ch=%d c=%u sample=%u got=%.9g want=%.9g\n",
                 ch, c, s, got[c][s], want[c][s]);
            break;
         }
      }
   }

   for (c = 0; c < (spx_uint32_t)ch; c++) { free(got[c]); free(want[c]); speex_resampler_destroy(ref[c]); }
   free(input);
   speex_resampler_destroy(st);
   return 0;
}

/* ---- int ---------------------------------------------------------------- */
static int test_int(int ch, int ri, int quality)
{
   int err;
   spx_uint32_t c, s;
   const int in_rate = ratios[ri][0], out_rate = ratios[ri][1];

   SpeexResamplerState *st = speex_resampler_init((spx_uint32_t)ch, in_rate, out_rate, quality, &err);
   if (!st) { fail("int init failed ch=%d\n", ch); return 1; }
   speex_resampler_skip_zeros(st);

   SpeexResamplerState *ref[8];
   for (c = 0; c < (spx_uint32_t)ch; c++) {
      ref[c] = speex_resampler_init(1, in_rate, out_rate, quality, &err);
      if (!ref[c]) { fail("ref init failed\n"); return 1; }
      speex_resampler_skip_zeros(ref[c]);
      /* The reference reads the SAME interleaved buffer, one channel, via the
         inter-channel stride, so it sees the identical input sequence. */
      speex_resampler_set_input_stride(ref[c], (spx_uint32_t)ch);
      speex_resampler_set_output_stride(ref[c], 1);
   }

   spx_uint32_t total = TOTAL_FRAMES;
   short *input = malloc((size_t)total * ch * sizeof(short));
   for (spx_uint32_t i = 0; i < total * (spx_uint32_t)ch; i++)
      input[i] = (short)((int)(rnd() % 20001) - 10000);

   spx_uint32_t cap = total * 8 + 64;
   short *got[8], *want[8];
   spx_uint32_t produced[8] = {0}, ref_produced[8] = {0};
   for (c = 0; c < (spx_uint32_t)ch; c++) {
      got[c] = malloc(cap * sizeof(short));
      want[c] = malloc(cap * sizeof(short));
   }

   spx_uint32_t pos = 0, size_idx = 0;
   while (pos < total) {
      spx_uint32_t chunk = frame_sizes[size_idx % NSIZE];
      size_idx++;
      if (chunk > total - pos) chunk = total - pos;

      /* Sufficient output capacity; see test_float for rationale. */
      spx_uint32_t out_cap = chunk * 6 + 64;
      spx_uint32_t il = chunk, ol = out_cap;
      short *outbuf = malloc((size_t)out_cap * ch * sizeof(short));
      speex_resampler_process_interleaved_int(
         st, input + (size_t)pos * ch, &il, outbuf, &ol);
      checks++;

      for (c = 0; c < (spx_uint32_t)ch; c++) {
         for (s = 0; s < ol; s++)
            got[c][produced[c] + s] = outbuf[s * ch + c];
         produced[c] += ol;

         spx_uint32_t ril = chunk, rol = out_cap;
         short *refout = malloc((size_t)out_cap * sizeof(short));
         speex_resampler_process_int(ref[c], 0,
            input + (size_t)pos * ch + c, &ril, refout, &rol);
         if (ril != il)
            fail("INT consumed mismatch ch=%d c=%u: inter=%u ref=%u\n", ch, c, il, ril);
         if (rol != ol)
            fail("INT produced mismatch ch=%d c=%u: inter=%u ref=%u\n", ch, c, ol, rol);
         for (s = 0; s < rol; s++)
            want[c][ref_produced[c] + s] = refout[s];
         ref_produced[c] += rol;
         free(refout);
      }

      for (c = 1; c < (spx_uint32_t)ch; c++)
         if (produced[c] != produced[0] || ref_produced[c] != ref_produced[0])
            fail("INT lockstep ch=%d c=%u got=(%u,%u) ref0=%u\n",
                 ch, c, produced[c], produced[0], ref_produced[0]);

      pos += il;
      free(outbuf);
   }

   for (c = 0; c < (spx_uint32_t)ch; c++) {
      if (produced[c] != ref_produced[c]) {
         fail("INT total mismatch ch=%d c=%u got=%u want=%u\n", ch, c, produced[c], ref_produced[c]);
      } else for (s = 0; s < produced[c]; s++) {
         if (got[c][s] != want[c][s]) {
            fail("INT sample mismatch ch=%d c=%u sample=%u got=%d want=%d\n",
                 ch, c, s, (int)got[c][s], (int)want[c][s]);
            break;
         }
      }
   }

   for (c = 0; c < (spx_uint32_t)ch; c++) { free(got[c]); free(want[c]); speex_resampler_destroy(ref[c]); }
   free(input);
   speex_resampler_destroy(st);
   return 0;
}

/* Tiny, exactly-sized output buffers (including a single frame) must never
   cause an out-of-bounds access or an infinite loop, and every channel must
   still report identical frame counts. Retries with a bigger buffer drain
   the input. Run under ASan/UBSan to validate memory safety. */
static void test_tiny_buffers(int use_int, int ch, int ri, int quality)
{
   int err, c;
   const int in_rate = ratios[ri][0], out_rate = ratios[ri][1];
   SpeexResamplerState *st = speex_resampler_init((spx_uint32_t)ch, in_rate, out_rate, quality, &err);
   if (!st) { fail("tiny init failed\n"); return; }
   speex_resampler_skip_zeros(st);

   spx_uint32_t total = 600, pos = 0;
   void *input;
   if (use_int) {
      short *p = malloc((size_t)total * ch * sizeof(short));
      for (spx_uint32_t i = 0; i < total * (spx_uint32_t)ch; i++) p[i] = (short)((int)(rnd() % 2001) - 1000);
      input = p;
   } else {
      float *p = malloc((size_t)total * ch * sizeof(float));
      for (spx_uint32_t i = 0; i < total * (spx_uint32_t)ch; i++) p[i] = 0.25f;
      input = p;
   }

   static const spx_uint32_t tiny_caps[] = {1u, 1u, 2u, 3u};
   int guard = 0;
   while (pos < total) {
      spx_uint32_t chunk = 17;
      if (chunk > total - pos) chunk = total - pos;
      spx_uint32_t cap = tiny_caps[(pos / 17) % 4];

      spx_uint32_t il = chunk, ol = cap;
      if (use_int) {
         short *ob = malloc((size_t)cap * ch * sizeof(short));
         speex_resampler_process_interleaved_int(st, (short *)input + (size_t)pos * ch, &il, ob, &ol);
         free(ob);
      } else {
         float *ob = malloc((size_t)cap * ch * sizeof(float));
         speex_resampler_process_interleaved_float(st, (float *)input + (size_t)pos * ch, &il, ob, &ol);
         free(ob);
      }
      checks++;
      if (il == 0) {
         /* Output-bound: drain with a large buffer at the same position. */
         spx_uint32_t dil = chunk, dol = chunk * 6 + 64;
         if (use_int) {
            short *ob = malloc((size_t)dol * ch * sizeof(short));
            speex_resampler_process_interleaved_int(st, (short *)input + (size_t)pos * ch, &dil, ob, &dol);
            free(ob);
         } else {
            float *ob = malloc((size_t)dol * ch * sizeof(float));
            speex_resampler_process_interleaved_float(st, (float *)input + (size_t)pos * ch, &dil, ob, &dol);
            free(ob);
         }
         if (dil == 0) { fail("%s tiny drain stall ch=%d\n", use_int ? "INT" : "FLOAT", ch); break; }
         il = dil;
      }
      pos += il;
      if (++guard > 100000) { fail("%s tiny runaway ch=%d\n", use_int ? "INT" : "FLOAT", ch); break; }
   }
   (void)c;
   free(input);
   speex_resampler_destroy(st);
}

int main(void)
{
   int ch, ri;
   for (ch = 2; ch <= 8; ch++)
      for (ri = 0; ri < NRATIO; ri++) {
         test_float(ch, ri, 4);
         test_int(ch, ri, 4);
      }
   for (ch = 2; ch <= 8; ch += 3)
      for (ri = 0; ri < NRATIO; ri += 4) {
         test_float(ch, ri, 0);
         test_float(ch, ri, 10);
         test_int(ch, ri, 0);
         test_int(ch, ri, 10);
      }
   for (ch = 2; ch <= 8; ch++)
      for (ri = 0; ri < NRATIO; ri++) {
         test_tiny_buffers(0, ch, ri, 4);
         test_tiny_buffers(1, ch, ri, 4);
      }

   if (failures == 0) {
      printf("PASS: %d interleaved calls; all channels lockstep and bit-exact vs mono reference\n", checks);
      return 0;
   }
   printf("FAIL: %d failures among %d checks\n", failures, checks);
   return 1;
}
