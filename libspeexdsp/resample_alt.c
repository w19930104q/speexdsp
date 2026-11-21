/* Compiles the resampler a second time, standalone (OUTSIDE_SPEEX) with
   renamed symbols, in the arithmetic mode OPPOSITE to the one the library
   was configured with (the -DFIXED_POINT/-DFLOATING_POINT flag comes from
   libtestalt_la_CPPFLAGS in Makefile.am). This lets testresample_consistency
   compare fixed-point against floating-point within a single make check
   binary, whichever way the library itself was built.

   This file must be compiled WITHOUT HAVE_CONFIG_H: config.h would define
   the library's own arithmetic mode and clash with the opposite one. */
#define OUTSIDE_SPEEX
#define RANDOM_PREFIX testalt
#include "resample.c"
