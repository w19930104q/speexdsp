# CTest script: verify the CMake build version matches configure.ac and the
# generated speexdsp.pc. Invoked with:
#   cmake -DCONFIGURE_AC=... -DPC_FILE=... -DEXPECT_VERSION=... -P version-check.cmake

if(NOT EXISTS "${CONFIGURE_AC}")
  message(FATAL_ERROR "configure.ac not found: ${CONFIGURE_AC}")
endif()
file(READ "${CONFIGURE_AC}" _ac)
string(REGEX MATCH "AC_INIT\\(\\[speexdsp\\],[ \t]*\\[([0-9][0-9.]*)\\]" _ "${_ac}")
set(_ac_version "${CMAKE_MATCH_1}")
if(NOT _ac_version)
  message(FATAL_ERROR "could not parse version from ${CONFIGURE_AC}")
endif()

if(NOT _ac_version STREQUAL EXPECT_VERSION)
  message(FATAL_ERROR
    "version mismatch: configure.ac says ${_ac_version}, "
    "CMake build uses ${EXPECT_VERSION}")
endif()

if(NOT EXISTS "${PC_FILE}")
  message(FATAL_ERROR "speexdsp.pc not found: ${PC_FILE}")
endif()
file(READ "${PC_FILE}" _pc)
string(REGEX MATCH "Version:[ \t]*([0-9][0-9.]*)" _ "${_pc}")
if(NOT CMAKE_MATCH_1 STREQUAL _ac_version)
  message(FATAL_ERROR
    "version mismatch: configure.ac says ${_ac_version}, "
    "speexdsp.pc says ${CMAKE_MATCH_1}")
endif()

message(STATUS "version-sync OK: ${_ac_version}")
