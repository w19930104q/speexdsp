# CMake build for SpeexDSP.
#
# This mirrors the autotools build (configure.ac / Makefile.am) and is meant
# for downstreams that only speak CMake (Android NDK, embedded SDKs, ...).
# Version numbers are NOT duplicated here: they are parsed from configure.ac
# so the two build systems can never drift apart. The ctest "version-sync"
# double-checks this.

cmake_minimum_required(VERSION 3.10)

# ---------------------------------------------------------------------------
# Version information, parsed from configure.ac (single source of truth)
# ---------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/configure.ac" _configure_ac)

string(REGEX MATCH "AC_INIT\\(\\[speexdsp\\],[ \t]*\\[([0-9][0-9.]*)\\]"
       _ "${_configure_ac}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "Could not parse package version from configure.ac")
endif()
set(SPEEXDSP_VERSION "${CMAKE_MATCH_1}")

foreach(_lt CURRENT REVISION AGE)
  string(REGEX MATCH "SPEEXDSP_LT_${_lt}=([0-9]+)" _ "${_configure_ac}")
  if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Could not parse SPEEXDSP_LT_${_lt} from configure.ac")
  endif()
  set(SPEEXDSP_LT_${_lt} "${CMAKE_MATCH_1}")
endforeach()

# libtool current:revision:age -> ELF soname, same rule libtool uses:
#   soname   = current - age
#   filename = (current - age).age.revision
math(EXPR SPEEXDSP_SOVERSION "${SPEEXDSP_LT_CURRENT} - ${SPEEXDSP_LT_AGE}")
set(SPEEXDSP_LIBVERSION
    "${SPEEXDSP_SOVERSION}.${SPEEXDSP_LT_AGE}.${SPEEXDSP_LT_REVISION}")

project(speexdsp VERSION ${SPEEXDSP_VERSION} LANGUAGES C)

# ---------------------------------------------------------------------------
# Options (named after their autotools counterparts)
# ---------------------------------------------------------------------------
option(SPEEXDSP_FIXED_POINT "Compile as fixed-point (--enable-fixed-point)" OFF)
option(SPEEXDSP_DISABLE_FLOAT_API
       "Disable the floating-point API (--disable-float-api)" OFF)
option(SPEEXDSP_FIXED_POINT_DEBUG
       "Debug fixed-point implementation (--enable-fixed-point-debug)" OFF)
option(SPEEXDSP_RESAMPLE_FULL_SINC_TABLE
       "Resample full SINC table (--enable-resample-full-sinc-table)" OFF)
option(SPEEXDSP_SSE "Use SSE/SSE2 optimizations when available" ON)
option(SPEEXDSP_NEON "Use NEON optimizations when available" ON)
option(SPEEXDSP_BUILD_EXAMPLES
       "Build example/test programs (--enable-examples)" ON)
# autotools builds shared by default; match that.
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

include(GNUInstallDirs)
include(CheckCSourceCompiles)
include(CheckIncludeFile)
include(CheckTypeSize)
include(CheckLibraryExists)
include(CTest)

# ---------------------------------------------------------------------------
# Feature checks, mirroring configure.ac
# ---------------------------------------------------------------------------
check_c_source_compiles("
int main(void) {
  int foo;
  foo = 10;
  int array[foo];
  return array[0];
}" VAR_ARRAYS)

set(_sse_src "
#include <xmmintrin.h>
__m128 testfunc(float *a, float *b) {
  return _mm_add_ps(_mm_loadu_ps(a), _mm_loadu_ps(b));
}
int main(void) { float a[4] = {0,0,0,0}, b[4] = {0,0,0,0}; testfunc(a, b); return 0; }")
set(_sse2_src "
#include <emmintrin.h>
__m128d testfunc(double *a, double *b) {
  return _mm_add_pd(_mm_loadu_pd(a), _mm_loadu_pd(b));
}
int main(void) { double a[2] = {0,0}, b[2] = {0,0}; testfunc(a, b); return 0; }")
set(_neon_src "
#include <arm_neon.h>
int32x4_t testfunc(int16_t *a, int16_t *b) {
      return vmull_s16(vld1_s16(a), vld1_s16(b));
}
int main(void) { int16_t a[4] = {0,0,0,0}, b[4] = {0,0,0,0}; testfunc(a, b); return 0; }")

# Like the autotools build, SSE is only used in floating-point builds.
if(SPEEXDSP_SSE AND NOT SPEEXDSP_FIXED_POINT)
  check_c_source_compiles("${_sse_src}" HAVE_SSE)
  check_c_source_compiles("${_sse2_src}" HAVE_SSE2)
endif()
if(SPEEXDSP_NEON)
  check_c_source_compiles("${_neon_src}" HAVE_NEON)
endif()

check_c_source_compiles("
#pragma GCC visibility push(hidden)
__attribute__((visibility(\"default\")))
int var=10;
int main(void) { return var; }" HAVE_VISIBILITY)
if(HAVE_VISIBILITY)
  set(EXPORT "__attribute__((visibility(\"default\")))")
else()
  set(EXPORT "")
endif()

check_include_file(stdint.h HAVE_STDINT_H)
check_include_file(inttypes.h HAVE_INTTYPES_H)
check_include_file(sys/types.h HAVE_SYS_TYPES_H)
if(HAVE_STDINT_H)
  set(INCLUDE_STDINT "#include <stdint.h>")
elseif(HAVE_INTTYPES_H)
  set(INCLUDE_STDINT "#include <inttypes.h>")
elseif(HAVE_SYS_TYPES_H)
  set(INCLUDE_STDINT "#include <sys/types.h>")
else()
  set(INCLUDE_STDINT "")
endif()

# Fixed-width types for speexdsp_config_types.h, same fallback order as
# configure.ac.
check_type_size("int16_t" SIZEOF_INT16_T)
check_type_size("uint16_t" SIZEOF_UINT16_T)
check_type_size("int32_t" SIZEOF_INT32_T)
check_type_size("uint32_t" SIZEOF_UINT32_T)
check_type_size("short" SIZEOF_SHORT)
check_type_size("int" SIZEOF_INT)
check_type_size("long" SIZEOF_LONG)

# Direct translation of the configure.ac case statements.
if(SIZEOF_INT16_T EQUAL 2)
  set(SIZE16 "int16_t")
elseif(SIZEOF_SHORT EQUAL 2)
  set(SIZE16 "short")
elseif(SIZEOF_INT EQUAL 2)
  set(SIZE16 "int")
endif()
if(SIZEOF_UINT16_T EQUAL 2)
  set(USIZE16 "uint16_t")
elseif(SIZEOF_SHORT EQUAL 2)
  set(USIZE16 "unsigned short")
elseif(SIZEOF_INT EQUAL 2)
  set(USIZE16 "unsigned int")
endif()
if(SIZEOF_INT32_T EQUAL 4)
  set(SIZE32 "int32_t")
elseif(SIZEOF_INT EQUAL 4)
  set(SIZE32 "int")
elseif(SIZEOF_LONG EQUAL 4)
  set(SIZE32 "long")
elseif(SIZEOF_SHORT EQUAL 4)
  set(SIZE32 "short")
endif()
if(SIZEOF_UINT32_T EQUAL 4)
  set(USIZE32 "uint32_t")
elseif(SIZEOF_INT EQUAL 4)
  set(USIZE32 "unsigned int")
elseif(SIZEOF_SHORT EQUAL 4)
  set(USIZE32 "unsigned short")
elseif(SIZEOF_LONG EQUAL 4)
  set(USIZE32 "unsigned long")
endif()
foreach(_t SIZE16 USIZE16 SIZE32 USIZE32)
  if(NOT ${_t})
    message(FATAL_ERROR "No type found for ${_t} on this platform!")
  endif()
endforeach()

check_library_exists(m cos "" HAVE_LIBM)
if(HAVE_LIBM)
  set(LIBM "-lm")
  set(LIBM_LINK m)
else()
  set(LIBM "")
  set(LIBM_LINK "")
endif()

# ---------------------------------------------------------------------------
# config.h equivalents
# ---------------------------------------------------------------------------
if(SPEEXDSP_FIXED_POINT)
  set(FIXED_POINT 1)
  set(USE_KISS_FFT 1)   # kiss_fft is the fixed-point default, as in configure.ac
  set(FFT_SOURCES libspeexdsp/kiss_fft.c libspeexdsp/kiss_fftr.c)
else()
  set(FLOATING_POINT 1)
  set(USE_SMALLFT 1)    # smallft is the floating-point default
  set(FFT_SOURCES libspeexdsp/smallft.c)
endif()
if(HAVE_SSE)
  set(USE_SSE 1)
endif()
if(HAVE_SSE2)
  set(USE_SSE2 1)
endif()
if(HAVE_NEON)
  set(USE_NEON 1)
endif()
if(SPEEXDSP_DISABLE_FLOAT_API)
  set(DISABLE_FLOAT_API 1)
endif()
if(SPEEXDSP_FIXED_POINT_DEBUG)
  set(FIXED_DEBUG 1)
endif()
if(SPEEXDSP_RESAMPLE_FULL_SINC_TABLE)
  set(RESAMPLE_FULL_SINC_TABLE 1)
endif()

configure_file(config.h.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/config.h @ONLY)
configure_file(include/speex/speexdsp_config_types.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/speex/speexdsp_config_types.h
               @ONLY)

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
add_library(speexdsp
  libspeexdsp/preprocess.c
  libspeexdsp/jitter.c
  libspeexdsp/mdf.c
  libspeexdsp/fftwrap.c
  libspeexdsp/filterbank.c
  libspeexdsp/resample.c
  libspeexdsp/buffer.c
  libspeexdsp/scal.c
  ${FFT_SOURCES})

set_target_properties(speexdsp PROPERTIES
  VERSION "${SPEEXDSP_LIBVERSION}"
  SOVERSION "${SPEEXDSP_SOVERSION}")
if(HAVE_VISIBILITY)
  set_target_properties(speexdsp PROPERTIES C_VISIBILITY_PRESET hidden)
endif()

target_compile_definitions(speexdsp PRIVATE HAVE_CONFIG_H)
target_include_directories(speexdsp
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libspeexdsp
    ${CMAKE_CURRENT_BINARY_DIR}
    # quoted #include "speexdsp_config_types.h" from the installed-style
    # headers resolves here, like -I$(top_builddir)/include/speex
    ${CMAKE_CURRENT_BINARY_DIR}/include/speex)
target_link_libraries(speexdsp PRIVATE ${LIBM_LINK})

# ---------------------------------------------------------------------------
# Example / test programs (autotools: BUILD_EXAMPLES noinst_PROGRAMS)
# ---------------------------------------------------------------------------
if(SPEEXDSP_BUILD_EXAMPLES)
  foreach(_test testdenoise testecho testjitter testresample testresample2)
    add_executable(${_test} libspeexdsp/${_test}.c)
    target_compile_definitions(${_test} PRIVATE HAVE_CONFIG_H)
    target_include_directories(${_test} PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}
      ${CMAKE_CURRENT_BINARY_DIR}/include/speex)
    target_link_libraries(${_test} PRIVATE speexdsp ${LIBM_LINK})
  endforeach()
  if(BUILD_TESTING)
    add_test(NAME testjitter COMMAND testjitter)
    add_test(NAME testresample COMMAND testresample)
    add_test(NAME testresample2 COMMAND testresample2)
  endif()
endif()

# Version-sync guard: fails if configure.ac and this build disagree.
if(BUILD_TESTING)
  add_test(NAME version-sync
    COMMAND ${CMAKE_COMMAND}
      -DCONFIGURE_AC=${CMAKE_CURRENT_SOURCE_DIR}/configure.ac
      -DPC_FILE=${CMAKE_CURRENT_BINARY_DIR}/speexdsp.pc
      -DEXPECT_VERSION=${SPEEXDSP_VERSION}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version-check.cmake)
endif()

# ---------------------------------------------------------------------------
# pkg-config (generated from the same speexdsp.pc.in as autotools)
# ---------------------------------------------------------------------------
set(prefix "${CMAKE_INSTALL_PREFIX}")
set(exec_prefix "\${prefix}")
set(libdir "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
set(includedir "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
set(PACKAGE_VERSION "${SPEEXDSP_VERSION}")
set(FFT_PKGCONFIG "")
configure_file(speexdsp.pc.in ${CMAKE_CURRENT_BINARY_DIR}/speexdsp.pc @ONLY)

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
install(TARGETS speexdsp
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(FILES
  include/speex/speex_echo.h
  include/speex/speex_jitter.h
  include/speex/speex_preprocess.h
  include/speex/speex_resampler.h
  include/speex/speexdsp_types.h
  ${CMAKE_CURRENT_BINARY_DIR}/include/speex/speexdsp_config_types.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/speex)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/speexdsp.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

message(STATUS "speexdsp ${SPEEXDSP_VERSION} "
  "(libtool ${SPEEXDSP_LT_CURRENT}:${SPEEXDSP_LT_REVISION}:${SPEEXDSP_LT_AGE}"
  " -> libspeexdsp.so.${SPEEXDSP_LIBVERSION})")
if(SPEEXDSP_FIXED_POINT)
  message(STATUS "  mode: fixed-point (kiss_fft)")
else()
  message(STATUS "  mode: floating-point (smallft)")
endif()
