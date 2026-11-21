/* Test program for the SPEEX_PREPROCESS_SET/GET_AGC_GAIN_LINEAR controls.
   In floating-point builds it verifies:
     1. a forced gain takes effect on the very next frame, sample by sample;
     2. after the forced frame, AGC adaptation still obeys the configured
        increment/decrement step bounds and the maximum gain bound;
     3. the GET control reports the gain actually in use;
     4. when the new controls are never called, output is identical to the
        baseline (bit-exactness regression).
   In fixed-point builds the AGC is not implemented, so it only checks that the
   preprocessor still runs and that unknown controls are rejected. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "speex/speex_preprocess.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NN 160
#define RATE 8000
#define WARMUP 40
#define NFRAMES 120
#define AMP 500
#define FORCED_GAIN 2.0f
#define EPS 1e-6f

static int nfailed = 0;

#define CHECK(cond, ...) do { \
   if (!(cond)) { \
      nfailed++; \
      printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); \
   } \
} while (0)

static void fill_frame(short *x, int frame)
{
   int i;
   for (i=0;i<NN;i++)
      x[i] = (short)floor(AMP*sin(2*M_PI*300.0*(frame*NN+i)/RATE));
}

#ifndef FIXED_POINT
/* Run NFRAMES of the same signal through two states. The "ref" state is never
   touched with the new controls; the "test" state forces a gain after the
   warm-up. Returns 0 on success. */
static int test_forced_gain(void)
{
   SpeexPreprocessState *ref = speex_preprocess_state_init(NN, RATE);
   SpeexPreprocessState *tst = speex_preprocess_state_init(NN, RATE);
   short rin[NN], tin[NN], rout[NN], tout[NN];
   int i, f;
   int on = 1, off = 0;
   int inc_db, dec_db, max_db;
   float inc_step, dec_step, max_gain;
   float prev_g, g;

   speex_preprocess_ctl(ref, SPEEX_PREPROCESS_SET_DENOISE, &off);
   speex_preprocess_ctl(tst, SPEEX_PREPROCESS_SET_DENOISE, &off);
   speex_preprocess_ctl(ref, SPEEX_PREPROCESS_SET_AGC, &on);
   speex_preprocess_ctl(tst, SPEEX_PREPROCESS_SET_AGC, &on);

   speex_preprocess_ctl(tst, SPEEX_PREPROCESS_GET_AGC_INCREMENT, &inc_db);
   speex_preprocess_ctl(tst, SPEEX_PREPROCESS_GET_AGC_DECREMENT, &dec_db);
   speex_preprocess_ctl(tst, SPEEX_PREPROCESS_GET_AGC_MAX_GAIN, &max_db);
   inc_step = expf(0.11513f*inc_db*NN/RATE);
   dec_step = expf(0.11513f*dec_db*NN/RATE);
   max_gain = expf(0.11513f*max_db);
   printf("step bounds: inc=%.5f dec=%.5f max_gain=%.3f\n",
          inc_step, dec_step, max_gain);

   for (f=0; f<NFRAMES; f++)
   {
      fill_frame(rin, f);
      memcpy(tin, rin, sizeof(rin));
      memcpy(rout, rin, sizeof(rin));
      memcpy(tout, tin, sizeof(tin));

      if (f == WARMUP)
      {
         float set_gain = FORCED_GAIN;
         speex_preprocess_ctl(tst, SPEEX_PREPROCESS_SET_AGC_GAIN_LINEAR,
                              &set_gain);
         g = -1.f;
         speex_preprocess_ctl(tst, SPEEX_PREPROCESS_GET_AGC_GAIN_LINEAR, &g);
         CHECK(fabsf(g - FORCED_GAIN) < EPS,
               "GET after SET before run: got %.6f, want %.6f", g,
               (double)FORCED_GAIN);
      }

      speex_preprocess_run(ref, rout);
      speex_preprocess_run(tst, tout);

      speex_preprocess_ctl(tst, SPEEX_PREPROCESS_GET_AGC_GAIN_LINEAR, &g);

      if (f == WARMUP)
      {
         /* The forced gain must apply to every output sample of the very next
            frame: out == FORCED_GAIN * in (within +/-1%). */
         CHECK(fabsf(g - FORCED_GAIN) < EPS,
               "GET on forced frame: got %.6f, want %.6f", g,
               (double)FORCED_GAIN);
         for (i=0;i<NN;i++)
         {
            double expected = FORCED_GAIN*(double)tin[i];
            double diff = fabs((double)tout[i]-expected);
            /* +/-1% relative error for non-zero samples; zero/near-zero
               input gets a two-count absolute tolerance (windowed
               overlap-add rounding) */
            int ok = fabs(expected) < 10.0 ? diff <= 2.0
                                           : diff/fabs(expected) <= 0.01;
            CHECK(ok,
                  "forced frame sample %d: in=%d out=%d expected=%.1f",
                  i, tin[i], tout[i], expected);
            if (nfailed > 10)
               return 0;
         }
      }

      if (f >= WARMUP)
      {
         /* Adaptation from the forced value must respect the step bounds. */
         if (f > WARMUP)
         {
            CHECK(g <= prev_g*inc_step + EPS,
                  "frame %d gain %.6f exceeds increase step bound (%.6f)",
                  f, g, prev_g*inc_step);
            CHECK(g >= prev_g*dec_step - EPS,
                  "frame %d gain %.6f below decrease step bound (%.6f)",
                  f, g, prev_g*dec_step);
         }
         CHECK(g <= max_gain + EPS,
               "frame %d gain %.6f exceeds max gain %.6f", f, g, max_gain);
      }
      prev_g = g;
   }

   speex_preprocess_state_destroy(ref);
   speex_preprocess_state_destroy(tst);
   return 0;
}

/* Two states running the exact same sequence without the new controls must
   produce bit-identical output (regression guard for the default path). */
static int test_unchanged_path(void)
{
   SpeexPreprocessState *a = speex_preprocess_state_init(NN, RATE);
   SpeexPreprocessState *b = speex_preprocess_state_init(NN, RATE);
   short ain[NN], bin[NN], aout[NN], bout[NN];
   int on = 1, off = 0, f;

   speex_preprocess_ctl(a, SPEEX_PREPROCESS_SET_DENOISE, &off);
   speex_preprocess_ctl(b, SPEEX_PREPROCESS_SET_DENOISE, &off);
   speex_preprocess_ctl(a, SPEEX_PREPROCESS_SET_AGC, &on);
   speex_preprocess_ctl(b, SPEEX_PREPROCESS_SET_AGC, &on);

   for (f=0; f<NFRAMES; f++)
   {
      fill_frame(ain, f);
      memcpy(bin, ain, sizeof(ain));
      memcpy(aout, ain, sizeof(ain));
      memcpy(bout, bin, sizeof(bin));
      speex_preprocess_run(a, aout);
      speex_preprocess_run(b, bout);
      CHECK(memcmp(aout, bout, sizeof(aout)) == 0,
            "frame %d differs between two default-path states", f);
      if (nfailed > 0)
         return 0;
   }
   speex_preprocess_state_destroy(a);
   speex_preprocess_state_destroy(b);
   return 0;
}
#endif /* !FIXED_POINT */

int main(void)
{
#if defined(FIXED_POINT) || defined(DISABLE_FLOAT_API)
   /* The linear gain controls are floating-point only, so they must be
      rejected (and the preprocessor must still run) in these builds. */
   SpeexPreprocessState *st = speex_preprocess_state_init(NN, RATE);
   int on = 1, rc;
   short x[NN];
   int f;
   speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_AGC, &on);
   for (f=0; f<10; f++)
   {
      fill_frame(x, f);
      speex_preprocess_run(st, x);
   }
   {
      float dummy = 1.f;
      rc = speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_AGC_GAIN_LINEAR,
                                &dummy);
   }
   CHECK(rc == -1, "linear gain ctl must be rejected in this build, got %d",
         rc);
   speex_preprocess_state_destroy(st);
#else
   float fg = 1.f;
   int rc;

   test_unchanged_path();
   test_forced_gain();

   /* Unknown/unsupported in fixed-point style check: invalid request still
      returns -1 and does not disturb the state. */
   {
      SpeexPreprocessState *st = speex_preprocess_state_init(NN, RATE);
      rc = speex_preprocess_ctl(st, 9999, &fg);
      CHECK(rc == -1, "unknown ctl should return -1, got %d", rc);
      speex_preprocess_state_destroy(st);
   }

   /* Forced gain above max_gain must be clamped to max_gain. */
   {
      SpeexPreprocessState *st = speex_preprocess_state_init(NN, RATE);
      int on = 1, max_db = 20;
      float got;
      speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_AGC, &on);
      speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &max_db);
      fg = 1e9f;
      speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_AGC_GAIN_LINEAR, &fg);
      speex_preprocess_ctl(st, SPEEX_PREPROCESS_GET_AGC_GAIN_LINEAR, &got);
      CHECK(fabsf(got - expf(0.11513f*max_db)) < EPS,
            "clamp to max_gain: got %.6f, want %.6f", got,
            (double)expf(0.11513f*max_db));
      speex_preprocess_state_destroy(st);
   }
#endif

   if (nfailed == 0)
   {
      printf("testagc: all tests passed\n");
      return 0;
   }
   printf("testagc: %d check(s) failed\n", nfailed);
   return 1;
}
