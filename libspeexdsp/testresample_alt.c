/* Shim TU: wraps the standalone (OUTSIDE_SPEEX) resampler whose symbols are
   renamed with RANDOM_PREFIX so they do not clash with the linked library.
   The arithmetic mode (fixed vs float) of that resampler is selected by the
   build flags of resample_alt.c, not of this file. */
#define OUTSIDE_SPEEX
#define RANDOM_PREFIX testalt
#include "speex/speex_resampler.h"
#include "testresample_alt.h"

/* Map the renamed struct type to the opaque wrapper type. The layout is
   identical; only the C-level type name differs. */
struct test_alt_state { char opaque[1]; };

static SpeexResamplerState *h(test_alt_state *st)
{
   return (SpeexResamplerState *)st;
}

test_alt_state *test_alt_init(unsigned nb_channels, unsigned in_rate,
                              unsigned out_rate, int quality, int *err)
{
   return (test_alt_state *)testalt_resampler_init(nb_channels, in_rate,
                                                   out_rate, quality, err);
}

test_alt_state *test_alt_init_frac(unsigned nb_channels, unsigned ratio_num,
                                   unsigned ratio_den, unsigned in_rate,
                                   unsigned out_rate, int quality, int *err)
{
   return (test_alt_state *)testalt_resampler_init_frac(nb_channels, ratio_num,
                                                        ratio_den, in_rate,
                                                        out_rate, quality, err);
}

void test_alt_destroy(test_alt_state *st)
{
   testalt_resampler_destroy(h(st));
}

int test_alt_process_int(test_alt_state *st, unsigned channel,
                         const short *in, unsigned *in_len,
                         short *out, unsigned *out_len)
{
   return testalt_resampler_process_int(h(st), channel,
               (const spx_int16_t *)in, (spx_uint32_t *)in_len,
               (spx_int16_t *)out, (spx_uint32_t *)out_len);
}

int test_alt_process_float(test_alt_state *st, unsigned channel,
                           const float *in, unsigned *in_len,
                           float *out, unsigned *out_len)
{
   return testalt_resampler_process_float(h(st), channel, in,
               (spx_uint32_t *)in_len, out, (spx_uint32_t *)out_len);
}

int test_alt_process_interleaved_int(test_alt_state *st, const short *in,
                                     unsigned *in_len, short *out,
                                     unsigned *out_len)
{
   return testalt_resampler_process_interleaved_int(h(st),
               (const spx_int16_t *)in, (spx_uint32_t *)in_len,
               (spx_int16_t *)out, (spx_uint32_t *)out_len);
}

int test_alt_process_interleaved_float(test_alt_state *st, const float *in,
                                       unsigned *in_len, float *out,
                                       unsigned *out_len)
{
   return testalt_resampler_process_interleaved_float(h(st), in,
               (spx_uint32_t *)in_len, out, (spx_uint32_t *)out_len);
}

int test_alt_reset_mem(test_alt_state *st)
{
   return testalt_resampler_reset_mem(h(st));
}

int test_alt_skip_zeros(test_alt_state *st)
{
   return testalt_resampler_skip_zeros(h(st));
}

int test_alt_set_quality(test_alt_state *st, int quality)
{
   return testalt_resampler_set_quality(h(st), quality);
}
