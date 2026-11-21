/* Wrapper exposing the prefix-renamed, standalone resampler (compiled in
   the opposite arithmetic mode from the library) through stable C types,
   so the consistency test never sees OUTSIDE_SPEEX/RANDOM_PREFIX macros. */
#ifndef TESTRESAMPLE_ALT_H
#define TESTRESAMPLE_ALT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct test_alt_state test_alt_state;

test_alt_state *test_alt_init(unsigned nb_channels, unsigned in_rate,
                              unsigned out_rate, int quality, int *err);
test_alt_state *test_alt_init_frac(unsigned nb_channels, unsigned ratio_num,
                                   unsigned ratio_den, unsigned in_rate,
                                   unsigned out_rate, int quality, int *err);
void test_alt_destroy(test_alt_state *st);
int test_alt_process_int(test_alt_state *st, unsigned channel,
                         const short *in, unsigned *in_len,
                         short *out, unsigned *out_len);
int test_alt_process_float(test_alt_state *st, unsigned channel,
                           const float *in, unsigned *in_len,
                           float *out, unsigned *out_len);
int test_alt_process_interleaved_int(test_alt_state *st, const short *in,
                                     unsigned *in_len, short *out,
                                     unsigned *out_len);
int test_alt_process_interleaved_float(test_alt_state *st, const float *in,
                                       unsigned *in_len, float *out,
                                       unsigned *out_len);
int test_alt_reset_mem(test_alt_state *st);
int test_alt_skip_zeros(test_alt_state *st);
int test_alt_set_quality(test_alt_state *st, int quality);

#ifdef __cplusplus
}
#endif
#endif
