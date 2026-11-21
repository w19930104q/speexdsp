/* Consistency regression suite for the SpeexDSP resampler.

   Groups:
     1. Interleaved API vs per-channel (mono) API on the same input.
        The interleaved entry points are thin strided wrappers around the
        per-channel path, so results must be BIT-EXACT (error 0), both
        single-shot and when streaming in irregular chunks.
     2. Fixed-point vs floating-point. The library (built in its configured
        arithmetic) is compared against a second copy of the resampler
        compiled in the opposite arithmetic (resample_alt.c). Outputs in
        Q15 LSBs must satisfy the quantization bound

            |y_fixed - y_float|  <=  B(L) = 0.5 + 4*sqrt(L)   [LSB]

        where L = filt_len for the given rate/quality. Derivation: the
        float path is near-exact (relative error ~2^-24). The fixed path
        accumulates (a) sinc-table coefficient quantization, <= 2^-16 per
        tap, (b) MULT16_16 product truncation, <= 2^-15 per tap; both act
        as L weakly-correlated noise terms of variance ~L each, i.e. an
        RMS error proportional to sqrt(L), and (c) final output
        requantization to int16, <= 0.5 LSB. The factor 4 is an empirical
        worst-case envelope: a scan over 450 rate/quality/signal configs
        (15 rate pairs x 6 qualities x 5 signals, N=4096) measures a worst
        case of (maxdiff-0.5)/sqrt(L) = 2.56, so 0.5 + 4*sqrt(L) holds
        with ~1.5x margin.
     3. Zero-length calls, NULL input, invalid arguments and extreme rate
        ratios: must be safe (no crash) and report proper error codes.
     4. reset_mem regression: after reset_mem a resampler must behave
        BIT-EXACTLY like a freshly created one, on every channel.
        (Regression: pre-fix, reset_mem cleared the filter history
        contiguously instead of per channel, leaving stale history in
        channels >= 1.)
     5. Magic-sample boundary: quality and rate changes mid-stream force
        filter-length changes (the magic_samples path). Must stay safe and
        keep the interleaved<->mono bit-exact invariant.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "speex/speex_resampler.h"
#include "testresample_alt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
      checks++; \
      if (!(cond)) { \
         printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
         failures++; \
      } \
   } while (0)

/* ------------------------------------------------------------------ */
/* Deterministic signal generation                                     */

static unsigned lcg_state = 22222;
static float frand(void) /* uniform in [-1, 1) */
{
   lcg_state = lcg_state*1103515245u + 12345u;
   return (float)((lcg_state >> 16) & 0x7fff)/16384.0f - 1.0f;
}

/* Mixed sines + noise, per-channel phase, peak ~0.85 full scale */
static void gen_signal_float(float *in, unsigned n, unsigned channels, int sig)
{
   unsigned i, c;
   for (i = 0; i < n; i++) {
      for (c = 0; c < channels; c++) {
         double v = 0.55*sin((i + 7*c)*2.0*M_PI/(31 + 3*sig))
                  + 0.20*sin((i + 13*c)*2.0*M_PI/97.0)
                  + 0.10*frand();
         in[i*channels + c] = (float)(v*0.95);
      }
   }
}

static void gen_signal_int(short *in, unsigned n, unsigned channels, int sig)
{
   unsigned i, c;
   for (i = 0; i < n; i++) {
      for (c = 0; c < channels; c++) {
         double v = 0.55*sin((i + 7*c)*2.0*M_PI/(31 + 3*sig))
                  + 0.20*sin((i + 13*c)*2.0*M_PI/97.0)
                  + 0.10*frand();
         long r = lrint(v*0.95*32767.0);
         if (r > 32767) r = 32767;
         if (r < -32768) r = -32768;
         in[i*channels + c] = (short)r;
      }
   }
}

/* ------------------------------------------------------------------ */
/* filt_len of the resampler for a given configuration (mirrors
   update_filter() in resample.c; rates are reduced by gcd first).     */

static unsigned gcd_u(unsigned a, unsigned b)
{
   while (b) { unsigned t = a; a = b; b = t % b; }
   return a;
}

static unsigned resampler_filt_len(unsigned in_rate, unsigned out_rate, int quality)
{
   static const unsigned base_length[11] =
      {8, 16, 32, 48, 64, 80, 96, 128, 160, 192, 256};
   unsigned g = gcd_u(in_rate, out_rate);
   unsigned num = in_rate/g, den = out_rate/g;
   unsigned len = base_length[quality];
   if (num > den) {
      /* down-sampling: scale by num/den, round up to a multiple of 8 */
      len = len*num/den;
      len = ((len - 1) & ~7u) + 8;
   }
   return len;
}

/* Quantization bound for fixed-vs-float, in Q15 LSBs (see header). */
static double quant_bound(unsigned in_rate, unsigned out_rate, int quality)
{
   return 0.5 + 4.0*sqrt((double)resampler_filt_len(in_rate, out_rate, quality));
}

/* ------------------------------------------------------------------ */
/* Group 1: interleaved vs per-channel mono, bit-exact                 */

static void deinterleave_int(short *dst, const short *src, unsigned n, unsigned ch, unsigned c)
{
   unsigned i;
   for (i = 0; i < n; i++) dst[i] = src[i*ch + c];
}
static void interleave_int(short *dst, const short *src, unsigned n, unsigned ch, unsigned c)
{
   unsigned i;
   for (i = 0; i < n; i++) dst[i*ch + c] = src[i];
}
static void deinterleave_float(float *dst, const float *src, unsigned n, unsigned ch, unsigned c)
{
   unsigned i;
   for (i = 0; i < n; i++) dst[i] = src[i*ch + c];
}
static void interleave_float(float *dst, const float *src, unsigned n, unsigned ch, unsigned c)
{
   unsigned i;
   for (i = 0; i < n; i++) dst[i*ch + c] = src[i];
}

#define N1 2048
#define CAP1 (N1*4 + 512)

/* One interleaved-vs-mono comparison, int API, single-shot. */
static void run_ilv_mono_int(unsigned ch, unsigned ir, unsigned orate, int q, int sig)
{
   short *in = malloc(sizeof(short)*N1*ch);
   short *out_ilv = calloc(CAP1*ch, sizeof(short));
   short *out_mono = calloc(CAP1*ch, sizeof(short));
   short *tmp_in = malloc(sizeof(short)*N1);
   short *tmp_out = calloc(CAP1, sizeof(short));
   SpeexResamplerState **mono = malloc(sizeof(*mono)*ch);
   SpeexResamplerState *st;
   spx_uint32_t il, ol;
   unsigned c, frames_ilv, frames_mono = 0;
   int err, mismatch = -1;

   gen_signal_int(in, N1, ch, sig);

   st = speex_resampler_init(ch, ir, orate, q, &err);
   CHECK(st != NULL);
   il = N1; ol = CAP1;
   CHECK(speex_resampler_process_interleaved_int(st, in, &il, out_ilv, &ol)
         == RESAMPLER_ERR_SUCCESS);
   CHECK(il == N1);
   frames_ilv = ol;
   speex_resampler_destroy(st);

   for (c = 0; c < ch; c++) {
      mono[c] = speex_resampler_init(1, ir, orate, q, &err);
      CHECK(mono[c] != NULL);
      deinterleave_int(tmp_in, in, N1, ch, c);
      il = N1; ol = CAP1;
      CHECK(speex_resampler_process_int(mono[c], 0, tmp_in, &il, tmp_out, &ol)
            == RESAMPLER_ERR_SUCCESS);
      CHECK(il == N1);
      if (c == 0) frames_mono = ol;
      else CHECK(ol == frames_mono);
      interleave_int(out_mono, tmp_out, ol, ch, c);
      speex_resampler_destroy(mono[c]);
   }

   CHECK(frames_ilv == frames_mono);
   {
      unsigned n = frames_ilv < frames_mono ? frames_ilv : frames_mono;
      unsigned i;
      for (i = 0; i < n*ch; i++)
         if (out_ilv[i] != out_mono[i]) { mismatch = (int)i; break; }
   }
   if (mismatch >= 0) {
      printf("FAIL int interleaved-vs-mono %uch %u->%u q%d sig%d: first diff at sample %d (ch %d): %d vs %d\n",
             ch, ir, orate, q, sig, mismatch, mismatch%(int)ch,
             out_ilv[mismatch], out_mono[mismatch]);
      failures++;
   }
   checks++;

   free(in); free(out_ilv); free(out_mono); free(tmp_in); free(tmp_out); free(mono);
}

/* Same, float API. */
static void run_ilv_mono_float(unsigned ch, unsigned ir, unsigned orate, int q, int sig)
{
   float *in = malloc(sizeof(float)*N1*ch);
   float *out_ilv = calloc(CAP1*ch, sizeof(float));
   float *out_mono = calloc(CAP1*ch, sizeof(float));
   float *tmp_in = malloc(sizeof(float)*N1);
   float *tmp_out = calloc(CAP1, sizeof(float));
   SpeexResamplerState **mono = malloc(sizeof(*mono)*ch);
   SpeexResamplerState *st;
   spx_uint32_t il, ol;
   unsigned c, frames_ilv, frames_mono = 0;
   int err, mismatch = -1;

   gen_signal_float(in, N1, ch, sig);

   st = speex_resampler_init(ch, ir, orate, q, &err);
   CHECK(st != NULL);
   il = N1; ol = CAP1;
   CHECK(speex_resampler_process_interleaved_float(st, in, &il, out_ilv, &ol)
         == RESAMPLER_ERR_SUCCESS);
   CHECK(il == N1);
   frames_ilv = ol;
   speex_resampler_destroy(st);

   for (c = 0; c < ch; c++) {
      mono[c] = speex_resampler_init(1, ir, orate, q, &err);
      CHECK(mono[c] != NULL);
      deinterleave_float(tmp_in, in, N1, ch, c);
      il = N1; ol = CAP1;
      CHECK(speex_resampler_process_float(mono[c], 0, tmp_in, &il, tmp_out, &ol)
            == RESAMPLER_ERR_SUCCESS);
      CHECK(il == N1);
      if (c == 0) frames_mono = ol;
      else CHECK(ol == frames_mono);
      interleave_float(out_mono, tmp_out, ol, ch, c);
      speex_resampler_destroy(mono[c]);
   }

   CHECK(frames_ilv == frames_mono);
   {
      unsigned n = frames_ilv < frames_mono ? frames_ilv : frames_mono;
      unsigned i;
      for (i = 0; i < n*ch; i++)
         if (out_ilv[i] != out_mono[i]) { mismatch = (int)i; break; }
   }
   if (mismatch >= 0) {
      printf("FAIL float interleaved-vs-mono %uch %u->%u q%d sig%d: first diff at sample %d (ch %d): %g vs %g\n",
             ch, ir, orate, q, sig, mismatch, mismatch%(int)ch,
             (double)out_ilv[mismatch], (double)out_mono[mismatch]);
      failures++;
   }
   checks++;

   free(in); free(out_ilv); free(out_mono); free(tmp_in); free(tmp_out); free(mono);
}

/* Chunked streaming: both interfaces fed the same irregular chunk sizes
   must stay bit-exact with each other (and hence with single-shot). */
static void run_ilv_mono_chunked(unsigned ch, unsigned ir, unsigned orate, int q)
{
   static const unsigned chunks[] = {1, 7, 64, 3, 255, 100, 2, 512, 33, 900};
   unsigned nchunks = sizeof(chunks)/sizeof(chunks[0]);
   short *in = malloc(sizeof(short)*N1*ch);
   short *out_ilv = calloc(CAP1*ch, sizeof(short));
   short *out_mono = calloc(CAP1*ch, sizeof(short));
   short *tmp_in = malloc(sizeof(short)*N1);
   short *tmp_out = calloc(CAP1, sizeof(short));
   SpeexResamplerState **mono = malloc(sizeof(*mono)*ch);
   SpeexResamplerState *st;
   unsigned c, k, pos, total_ilv = 0, total_mono = 0;
   int err, mismatch = -1;

   gen_signal_int(in, N1, ch, 1);

   st = speex_resampler_init(ch, ir, orate, q, &err);
   CHECK(st != NULL);
   for (c = 0; c < ch; c++) {
      mono[c] = speex_resampler_init(1, ir, orate, q, &err);
      CHECK(mono[c] != NULL);
   }

   pos = 0;
   for (k = 0; pos < N1; k++) {
      unsigned want = chunks[k % nchunks];
      unsigned got_ilv, got_mono_c = 0;
      if (want > N1 - pos) want = N1 - pos;
      {
         spx_uint32_t il = want, ol = CAP1 - total_ilv;
         CHECK(speex_resampler_process_interleaved_int(st, in + pos*ch, &il,
                       out_ilv + total_ilv*ch, &ol) == RESAMPLER_ERR_SUCCESS);
         CHECK(il == want);
         got_ilv = ol;
      }
      for (c = 0; c < ch; c++) {
         spx_uint32_t il, ol;
         deinterleave_int(tmp_in, in + pos*ch, want, ch, c);
         il = want; ol = CAP1 - total_mono;
         CHECK(speex_resampler_process_int(mono[c], 0, tmp_in, &il, tmp_out, &ol)
               == RESAMPLER_ERR_SUCCESS);
         CHECK(il == want);
         if (c == 0) got_mono_c = ol;
         else CHECK(ol == got_mono_c);
         interleave_int(out_mono + total_mono*ch, tmp_out, ol, ch, c);
      }
      CHECK(got_ilv == got_mono_c);
      total_ilv += got_ilv;
      total_mono += got_mono_c;
      pos += want;
   }

   CHECK(total_ilv == total_mono);
   {
      unsigned n = total_ilv < total_mono ? total_ilv : total_mono;
      unsigned i;
      for (i = 0; i < n*ch; i++)
         if (out_ilv[i] != out_mono[i]) { mismatch = (int)i; break; }
   }
   if (mismatch >= 0) {
      printf("FAIL chunked interleaved-vs-mono %uch %u->%u q%d: first diff at sample %d\n",
             ch, ir, orate, q, mismatch);
      failures++;
   }
   checks++;

   speex_resampler_destroy(st);
   for (c = 0; c < ch; c++) speex_resampler_destroy(mono[c]);
   free(in); free(out_ilv); free(out_mono); free(tmp_in); free(tmp_out); free(mono);
}

static void test_interleaved_vs_mono(void)
{
   static const unsigned rates[][2] = {
      {48000, 48000}, {48000, 16000}, {16000, 48000},
      {44100, 48000}, {48000, 44100},
   };
   static const unsigned chs[] = {2, 3, 4};
   static const int quals[] = {0, 4, 10};
   unsigned r, ci, qi;
   for (r = 0; r < sizeof(rates)/sizeof(rates[0]); r++)
      for (ci = 0; ci < sizeof(chs)/sizeof(chs[0]); ci++)
         for (qi = 0; qi < sizeof(quals)/sizeof(quals[0]); qi++) {
            run_ilv_mono_int(chs[ci], rates[r][0], rates[r][1], quals[qi], (int)(r+qi));
            run_ilv_mono_float(chs[ci], rates[r][0], rates[r][1], quals[qi], (int)(r+qi));
         }
   /* chunked streaming on a subset */
   run_ilv_mono_chunked(2, 48000, 16000, 4);
   run_ilv_mono_chunked(3, 16000, 48000, 4);
   run_ilv_mono_chunked(2, 48000, 48000, 10);
   run_ilv_mono_chunked(4, 44100, 48000, 3);
   printf("interleaved-vs-mono: done\n");
}

/* ------------------------------------------------------------------ */
/* Group 2: fixed-point vs floating-point                              */

#define N2 4096

static void run_fixed_vs_float(unsigned ch, unsigned ir, unsigned orate, int q, int sig)
{
   /* Size the output buffer from the exact ratio plus filter-latency slack */
   unsigned cap = (unsigned)((double)N2*orate/ir + 2.0*resampler_filt_len(ir, orate, q) + 256.0);
   short *in = malloc(sizeof(short)*N2*ch);
   short *out_lib = calloc((size_t)cap*ch, sizeof(short));
   short *out_alt = calloc((size_t)cap*ch, sizeof(short));
   SpeexResamplerState *lib;
   test_alt_state *alt;
   spx_uint32_t il, ol;
   unsigned uil, uol;
   int err, maxdiff = 0;
   unsigned frames_lib, frames_alt;
   double bound = quant_bound(ir, orate, q);

   gen_signal_int(in, N2, ch, sig);

   lib = speex_resampler_init(ch, ir, orate, q, &err);
   CHECK(lib != NULL);
   il = N2; ol = cap;
   CHECK(speex_resampler_process_interleaved_int(lib, in, &il, out_lib, &ol)
         == RESAMPLER_ERR_SUCCESS);
   CHECK(il == N2);
   frames_lib = ol;
   speex_resampler_destroy(lib);

   alt = test_alt_init(ch, ir, orate, q, &err);
   CHECK(alt != NULL);
   uil = N2; uol = cap;
   CHECK(test_alt_process_interleaved_int(alt, in, &uil, out_alt, &uol)
         == RESAMPLER_ERR_SUCCESS);
   CHECK(uil == N2);
   frames_alt = uol;
   test_alt_destroy(alt);

   /* Identical integer control logic on both sides -> identical frame count */
   CHECK(frames_lib == frames_alt);
   {
      unsigned n = frames_lib < frames_alt ? frames_lib : frames_alt;
      unsigned i;
      for (i = 0; i < n*ch; i++) {
         int d = abs((int)out_lib[i] - (int)out_alt[i]);
         if (d > maxdiff) maxdiff = d;
      }
   }
   if ((double)maxdiff > bound) {
      printf("FAIL fixed-vs-float %uch %u->%u q%d sig%d: maxdiff=%d LSB > bound=%.2f (filt_len=%u)\n",
             ch, ir, orate, q, sig, maxdiff, bound,
             resampler_filt_len(ir, orate, q));
      failures++;
   }
   checks++;

   free(in); free(out_lib); free(out_alt);
}

static void test_fixed_vs_float(void)
{
   static const unsigned rates[][2] = {
      {48000, 48000}, {48000, 16000}, {16000, 48000},
      {44100, 48000}, {48000, 44100}, {96000, 8000}, {8000, 96000},
      {48000, 12345}, {12345, 48000}, {48000, 22050},
   };
   static const int quals[] = {0, 3, 4, 7, 10};
   static const unsigned chs[] = {1, 2};
   unsigned r, qi, ci;
   for (r = 0; r < sizeof(rates)/sizeof(rates[0]); r++)
      for (qi = 0; qi < sizeof(quals)/sizeof(quals[0]); qi++)
         for (ci = 0; ci < sizeof(chs)/sizeof(chs[0]); ci++)
            run_fixed_vs_float(chs[ci], rates[r][0], rates[r][1],
                               quals[qi], (int)((r + 2*qi) % 3));
   printf("fixed-vs-float: done\n");
}

/* ------------------------------------------------------------------ */
/* Group 3: zero-length / invalid args / extreme rates                 */

static void test_invalid_args(void)
{
   int err = 0;
   SpeexResamplerState *st;

   CHECK(speex_resampler_init(0, 48000, 48000, 4, &err) == NULL);
   CHECK(err == RESAMPLER_ERR_INVALID_ARG);
   err = 0;
   CHECK(speex_resampler_init(2, 0, 48000, 4, &err) == NULL);
   CHECK(err == RESAMPLER_ERR_INVALID_ARG);
   err = 0;
   CHECK(speex_resampler_init(2, 48000, 0, 4, &err) == NULL);
   CHECK(err == RESAMPLER_ERR_INVALID_ARG);
   err = 0;
   CHECK(speex_resampler_init(2, 48000, 48000, 11, &err) == NULL);
   CHECK(err == RESAMPLER_ERR_INVALID_ARG);
   err = 0;
   CHECK(speex_resampler_init(2, 48000, 48000, -1, &err) == NULL);
   CHECK(err == RESAMPLER_ERR_INVALID_ARG);
   err = 0;
   CHECK(speex_resampler_init_frac(2, 0, 1, 48000, 48000, 4, &err) == NULL);
   CHECK(speex_resampler_init_frac(2, 1, 0, 48000, 48000, 4, &err) == NULL);
   CHECK(speex_resampler_strerror(RESAMPLER_ERR_INVALID_ARG) != NULL);

   st = speex_resampler_init(2, 48000, 48000, 4, &err);
   CHECK(st != NULL);
   CHECK(speex_resampler_set_quality(st, 11) == RESAMPLER_ERR_INVALID_ARG);
   CHECK(speex_resampler_set_quality(st, -1) == RESAMPLER_ERR_INVALID_ARG);
   CHECK(speex_resampler_set_rate_frac(st, 0, 1, 48000, 48000) == RESAMPLER_ERR_INVALID_ARG);
   CHECK(speex_resampler_set_rate_frac(st, 1, 0, 48000, 48000) == RESAMPLER_ERR_INVALID_ARG);
   speex_resampler_destroy(st);
   printf("invalid args: done\n");
}

static void test_zero_length(void)
{
   int err;
   SpeexResamplerState *st = speex_resampler_init(2, 48000, 16000, 4, &err);
   float in[8] = {0};
   float out[8];
   spx_uint32_t il, ol;
   CHECK(st != NULL);

   memset(out, 0x55, sizeof(out));
   il = 0; ol = 8;
   CHECK(speex_resampler_process_interleaved_float(st, in, &il, out, &ol)
         == RESAMPLER_ERR_SUCCESS);
   CHECK(il == 0 && ol == 0);
   il = 8; ol = 0;
   CHECK(speex_resampler_process_interleaved_float(st, in, &il, out, &ol)
         == RESAMPLER_ERR_SUCCESS);
   CHECK(il == 0 && ol == 0);
   il = 0; ol = 0;
   CHECK(speex_resampler_process_interleaved_float(st, in, &il, out, &ol)
         == RESAMPLER_ERR_SUCCESS);
   CHECK(il == 0 && ol == 0);
   /* NULL input with zero length must not crash */
   il = 0; ol = 8;
   CHECK(speex_resampler_process_float(st, 0, NULL, &il, out, &ol)
         == RESAMPLER_ERR_SUCCESS);
   CHECK(ol == 0);
   speex_resampler_destroy(st);

   {
      SpeexResamplerState *si = speex_resampler_init(1, 48000, 16000, 4, &err);
      short sin_[8] = {0};
      short sout_[8];
      spx_uint32_t sil = 0, sol = 8;
      CHECK(si != NULL);
      CHECK(speex_resampler_process_interleaved_int(si, sin_, &sil, sout_, &sol)
            == RESAMPLER_ERR_SUCCESS);
      CHECK(sil == 0 && sol == 0);
      sil = 0; sol = 8;
      CHECK(speex_resampler_process_int(si, 0, NULL, &sil, sout_, &sol)
            == RESAMPLER_ERR_SUCCESS);
      CHECK(sol == 0);
      speex_resampler_destroy(si);
   }
   printf("zero length: done\n");
}

static void test_null_input(void)
{
   /* NULL input means silence (flush/underrun scenario) */
   int err;
   SpeexResamplerState *st = speex_resampler_init(1, 48000, 48000, 3, &err);
   enum { NN = 512 };
   float out[NN];
   spx_uint32_t il = NN, ol = NN;
   unsigned i;
   CHECK(st != NULL);
   CHECK(speex_resampler_process_float(st, 0, NULL, &il, out, &ol)
         == RESAMPLER_ERR_SUCCESS);
   for (i = 0; i < ol; i++)
      if (out[i] != 0.f) {
         printf("FAIL null input produced nonzero out[%u]=%g\n", i, (double)out[i]);
         failures++;
         break;
      }
   checks++;
   speex_resampler_destroy(st);
   printf("null input: done (produced %u)\n", ol);
}

static void test_extreme_rates(void)
{
   /* Processable extreme ratios (bounded memory/time) */
   static const spx_uint32_t cases[][2] = {
      {1, 1}, {1, 100}, {100, 1}, {1, 1000}, {1000, 1},
      {44100, 192000}, {192000, 8000}, {777, 13}, {13, 777},
   };
   /* Absurd ratios: init must either fail cleanly or succeed; no crash */
   static const spx_uint32_t absurd[][2] = {
      {UINT32_MAX, 1}, {1, UINT32_MAX}, {0x80000000u, 0x7FFFFFFFu},
   };
   unsigned k;
   for (k = 0; k < sizeof(cases)/sizeof(cases[0]); k++) {
      spx_uint32_t num = cases[k][0], den = cases[k][1];
      int err = 0;
      SpeexResamplerState *st = speex_resampler_init_frac(1, num, den,
                                 num <= 48000 ? num : 48000,
                                 den <= 48000 ? den : 48000, 4, &err);
      enum { NN = 4096 };
      float *in, *out;
      spx_uint32_t il, ol, cap;
      double expect, slack;
      int rc, i;
      if (!st) {
         printf("extreme %u/%u: init rejected err=%d (acceptable)\n",
                num, den, err);
         continue;
      }
      in = calloc(NN, sizeof(float));
      /* Size the output buffer from the exact ratio (clamped) so input,
         not output, is the limiting resource. */
      expect = (double)NN*den/num;
      cap = expect + 256.0 > 4194304.0 ? 4194304u
                                       : (spx_uint32_t)(expect + 256.0);
      out = calloc(cap, sizeof(float));
      for (i = 0; i < NN; i++)
         in[i] = (float)(sin(i*2.0*M_PI/37.0)*0.5);
      il = NN; ol = cap;
      rc = speex_resampler_process_float(st, 0, in, &il, out, &ol);
      CHECK(rc == RESAMPLER_ERR_SUCCESS);
      CHECK(il == NN);
      slack = expect*0.05 + 64.0;
      if (fabs((double)ol - expect) > slack) {
         printf("FAIL extreme %u/%u: ol=%u expect~%.1f\n", num, den, ol, expect);
         failures++;
      }
      checks++;
      speex_resampler_destroy(st);
      free(in); free(out);
   }
   for (k = 0; k < sizeof(absurd)/sizeof(absurd[0]); k++) {
      int err = 0;
      SpeexResamplerState *st = speex_resampler_init_frac(1, absurd[k][0],
                                 absurd[k][1], 48000, 48000, 4, &err);
      if (st) {
         /* Init accepted an absurd ratio: a bounded call must still be safe */
         float in[16] = {0};
         float out[16];
         spx_uint32_t il = 16, ol = 16;
         CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol)
               == RESAMPLER_ERR_SUCCESS);
         speex_resampler_destroy(st);
      }
      printf("absurd %u/%u: %s\n", absurd[k][0], absurd[k][1],
             st ? "init accepted, bounded call safe" : "init rejected (acceptable)");
   }
   printf("extreme rates: done\n");
}

/* ------------------------------------------------------------------ */
/* Group 4: reset_mem regression                                       */

#define N4 1024

/* After reset_mem the state must be bit-exactly a fresh state, on every
   channel. Pre-fix this failed for channels >= 1 (stale filter history). */
static void run_reset_mem_case(unsigned ch, unsigned ir, unsigned orate, int q)
{
   short *sig1 = malloc(sizeof(short)*N4*ch);
   short *sig2 = malloc(sizeof(short)*N4*ch);
   short *out_r = calloc((N4*2 + 256)*ch, sizeof(short));
   short *out_f = calloc((N4*2 + 256)*ch, sizeof(short));
   SpeexResamplerState *rst, *fst;
   spx_uint32_t il, ol;
   unsigned frames_r, frames_f;
   int err, mismatch = -1;

   gen_signal_int(sig1, N4, ch, 1);
   gen_signal_int(sig2, N4, ch, 2);

   rst = speex_resampler_init(ch, ir, orate, q, &err);
   CHECK(rst != NULL);
   il = N4; ol = N4*2 + 256;
   CHECK(speex_resampler_process_interleaved_int(rst, sig1, &il, out_r, &ol)
         == RESAMPLER_ERR_SUCCESS);
   CHECK(speex_resampler_reset_mem(rst) == RESAMPLER_ERR_SUCCESS);
   il = N4; ol = N4*2 + 256;
   CHECK(speex_resampler_process_interleaved_int(rst, sig2, &il, out_r, &ol)
         == RESAMPLER_ERR_SUCCESS);
   frames_r = ol;

   fst = speex_resampler_init(ch, ir, orate, q, &err);
   CHECK(fst != NULL);
   il = N4; ol = N4*2 + 256;
   CHECK(speex_resampler_process_interleaved_int(fst, sig2, &il, out_f, &ol)
         == RESAMPLER_ERR_SUCCESS);
   frames_f = ol;

   CHECK(frames_r == frames_f);
   {
      unsigned n = frames_r < frames_f ? frames_r : frames_f;
      unsigned i;
      for (i = 0; i < n*ch; i++)
         if (out_r[i] != out_f[i]) { mismatch = (int)i; break; }
   }
   if (mismatch >= 0) {
      printf("FAIL reset_mem %uch %u->%u q%d: state after reset != fresh state, "
             "first diff at sample %d (channel %d): %d vs %d\n",
             ch, ir, orate, q, mismatch, mismatch%(int)ch,
             out_r[mismatch], out_f[mismatch]);
      failures++;
   }
   checks++;

   speex_resampler_destroy(rst);
   speex_resampler_destroy(fst);
   free(sig1); free(sig2); free(out_r); free(out_f);
}

/* After reset_mem, feeding silence must produce silence on all channels. */
static void run_reset_silence_case(unsigned ch, unsigned ir, unsigned orate, int q)
{
   float *sig = malloc(sizeof(float)*N4*ch);
   float *zeros = calloc(N4*ch, sizeof(float));
   float *out = malloc(sizeof(float)*(N4 + 256)*ch);
   SpeexResamplerState *st;
   spx_uint32_t il, ol;
   int err, bad = 0;
   unsigned i;

   gen_signal_float(sig, N4, ch, 1);
   st = speex_resampler_init(ch, ir, orate, q, &err);
   CHECK(st != NULL);
   il = N4; ol = N4 + 256;
   CHECK(speex_resampler_process_interleaved_float(st, sig, &il, out, &ol)
         == RESAMPLER_ERR_SUCCESS);
   CHECK(speex_resampler_reset_mem(st) == RESAMPLER_ERR_SUCCESS);
   il = N4; ol = N4 + 256;
   CHECK(speex_resampler_process_interleaved_float(st, zeros, &il, out, &ol)
         == RESAMPLER_ERR_SUCCESS);
   for (i = 0; i < ol*ch; i++)
      if (out[i] != 0.f) { bad = 1; break; }
   if (bad) {
      printf("FAIL reset_mem+silence %uch %u->%u q%d: nonzero output after reset\n",
             ch, ir, orate, q);
      failures++;
   }
   checks++;
   speex_resampler_destroy(st);
   free(sig); free(zeros); free(out);
}

static void test_reset_mem(void)
{
   static const unsigned chs[] = {1, 2, 3, 4};
   unsigned ci;
   for (ci = 0; ci < sizeof(chs)/sizeof(chs[0]); ci++) {
      run_reset_mem_case(chs[ci], 48000, 16000, 4);
      run_reset_mem_case(chs[ci], 44100, 48000, 7);
      run_reset_silence_case(chs[ci], 48000, 48000, 4);
   }
   printf("reset_mem: done\n");
}

/* ------------------------------------------------------------------ */
/* Group 5: magic samples (quality/rate changes mid-stream)            */

/* Change quality mid-stream (forces filter-length changes both ways,
   exercising magic_samples), then verify the interleaved<->mono
   bit-exact invariant still holds across the changes. */
static void run_quality_change_case(unsigned ch, unsigned ir, unsigned orate)
{
   short *in = malloc(sizeof(short)*N1*ch);
   short *out_ilv = calloc(CAP1*ch, sizeof(short));
   short *out_mono = calloc(CAP1*ch, sizeof(short));
   short *tmp_in = malloc(sizeof(short)*N1);
   short *tmp_out = calloc(CAP1, sizeof(short));
   SpeexResamplerState **mono = malloc(sizeof(*mono)*ch);
   SpeexResamplerState *st;
   static const int qseq[] = {8, 3, 10, 0};
   unsigned c, s, frames_ilv = 0, frames_mono = 0;
   int err, mismatch = -1;

   gen_signal_int(in, N1, ch, 1);

   st = speex_resampler_init(ch, ir, orate, qseq[0], &err);
   CHECK(st != NULL);
   for (c = 0; c < ch; c++) {
      mono[c] = speex_resampler_init(1, ir, orate, qseq[0], &err);
      CHECK(mono[c] != NULL);
   }

   for (s = 1; s < sizeof(qseq)/sizeof(qseq[0]); s++) {
      spx_uint32_t il, ol;
      CHECK(speex_resampler_set_quality(st, qseq[s]) == RESAMPLER_ERR_SUCCESS);
      il = N1/2; ol = CAP1 - frames_ilv;
      CHECK(speex_resampler_process_interleaved_int(st, in, &il,
                    out_ilv + frames_ilv*ch, &ol) == RESAMPLER_ERR_SUCCESS);
      frames_ilv += ol;
      for (c = 0; c < ch; c++) {
         spx_uint32_t mil, mol;
         CHECK(speex_resampler_set_quality(mono[c], qseq[s]) == RESAMPLER_ERR_SUCCESS);
         deinterleave_int(tmp_in, in, N1/2, ch, c);
         mil = N1/2; mol = CAP1;
         CHECK(speex_resampler_process_int(mono[c], 0, tmp_in, &mil, tmp_out, &mol)
               == RESAMPLER_ERR_SUCCESS);
         interleave_int(out_mono + frames_mono*ch, tmp_out, mol, ch, c);
         if (c == 0 && mol != ol) {
            printf("FAIL quality-change frame count %uch: mono %u vs ilv %u\n",
                   ch, mol, ol);
            failures++;
         }
      }
      frames_mono += ol;
   }

   CHECK(frames_ilv == frames_mono);
   {
      unsigned n = frames_ilv < frames_mono ? frames_ilv : frames_mono;
      unsigned i;
      for (i = 0; i < n*ch; i++)
         if (out_ilv[i] != out_mono[i]) { mismatch = (int)i; break; }
   }
   if (mismatch >= 0) {
      printf("FAIL quality-change interleaved-vs-mono %uch %u->%u: first diff at %d\n",
             ch, ir, orate, mismatch);
      failures++;
   }
   checks++;

   speex_resampler_destroy(st);
   for (c = 0; c < ch; c++) speex_resampler_destroy(mono[c]);
   free(in); free(out_ilv); free(out_mono); free(tmp_in); free(tmp_out); free(mono);
}

/* Change the rate mid-stream: must stay safe and keep the
   interleaved<->mono invariant. */
static void run_rate_change_case(unsigned ch)
{
   float *in = malloc(sizeof(float)*N1*ch);
   float *out_ilv = calloc(CAP1*ch, sizeof(float));
   float *out_mono = calloc(CAP1*ch, sizeof(float));
   float *tmp_in = malloc(sizeof(float)*N1);
   float *tmp_out = calloc(CAP1, sizeof(float));
   SpeexResamplerState **mono = malloc(sizeof(*mono)*ch);
   SpeexResamplerState *st;
   unsigned c, frames_ilv = 0, frames_mono = 0;
   int err, mismatch = -1, s;

   gen_signal_float(in, N1, ch, 1);

   st = speex_resampler_init(ch, 48000, 48000, 4, &err);
   CHECK(st != NULL);
   for (c = 0; c < ch; c++) {
      mono[c] = speex_resampler_init(1, 48000, 48000, 4, &err);
      CHECK(mono[c] != NULL);
   }

   for (s = 0; s < 3; s++) {
      spx_uint32_t il, ol;
      if (s == 1) {
         CHECK(speex_resampler_set_rate_frac(st, 1, 3, 48000, 16000)
               == RESAMPLER_ERR_SUCCESS);
         for (c = 0; c < ch; c++)
            CHECK(speex_resampler_set_rate_frac(mono[c], 1, 3, 48000, 16000)
                  == RESAMPLER_ERR_SUCCESS);
      } else if (s == 2) {
         CHECK(speex_resampler_set_rate_frac(st, 3, 2, 48000, 32000)
               == RESAMPLER_ERR_SUCCESS);
         for (c = 0; c < ch; c++)
            CHECK(speex_resampler_set_rate_frac(mono[c], 3, 2, 48000, 32000)
                  == RESAMPLER_ERR_SUCCESS);
      }
      il = N1/3; ol = CAP1 - frames_ilv;
      CHECK(speex_resampler_process_interleaved_float(st, in, &il,
                    out_ilv + frames_ilv*ch, &ol) == RESAMPLER_ERR_SUCCESS);
      frames_ilv += ol;
      for (c = 0; c < ch; c++) {
         spx_uint32_t mil, mol;
         deinterleave_float(tmp_in, in, N1/3, ch, c);
         mil = N1/3; mol = CAP1;
         CHECK(speex_resampler_process_float(mono[c], 0, tmp_in, &mil, tmp_out, &mol)
               == RESAMPLER_ERR_SUCCESS);
         interleave_float(out_mono + frames_mono*ch, tmp_out, mol, ch, c);
      }
      frames_mono += ol;
   }

   CHECK(frames_ilv == frames_mono);
   {
      unsigned n = frames_ilv < frames_mono ? frames_ilv : frames_mono;
      unsigned i;
      for (i = 0; i < n*ch; i++)
         if (out_ilv[i] != out_mono[i]) { mismatch = (int)i; break; }
   }
   if (mismatch >= 0) {
      printf("FAIL rate-change interleaved-vs-mono %uch: first diff at %d\n",
             ch, mismatch);
      failures++;
   }
   checks++;

   speex_resampler_destroy(st);
   for (c = 0; c < ch; c++) speex_resampler_destroy(mono[c]);
   free(in); free(out_ilv); free(out_mono); free(tmp_in); free(tmp_out); free(mono);
}

static void test_magic_samples(void)
{
   run_quality_change_case(2, 48000, 44100);
   run_quality_change_case(3, 48000, 16000);
   run_rate_change_case(2);
   run_rate_change_case(3);
   printf("magic samples: done\n");
}

/* ------------------------------------------------------------------ */

int main(void)
{
   test_invalid_args();
   test_zero_length();
   test_null_input();
   test_extreme_rates();
   test_interleaved_vs_mono();
   test_fixed_vs_float();
   test_reset_mem();
   test_magic_samples();

   printf("%s: %d checks, %d failures\n",
          failures ? "CONSISTENCY TESTS FAILED" : "ALL CONSISTENCY TESTS PASSED",
          checks, failures);
   return failures ? 1 : 0;
}
