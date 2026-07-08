cmake_minimum_required(VERSION 3.10)
project(keystone_examples C CXX ASM)

# check if SDK has been built and KEYSTONE_SDK_DIR configuration variable exists
if (NOT DEFINED KEYSTONE_SDK_DIR)
  message(FATAL_ERROR "Please set KEYSTONE_SDK_DIR configuration variable to an installed SDK path")
endif()

set(ENV{KEYSTONE_SDK_DIR} ${KEYSTONE_SDK_DIR})

if (DEFINED PATH)
  set(ENV{PATH} ${PATH})
endif()

include(${KEYSTONE_SDK_DIR}/cmake/macros.cmake)
include(ExternalProject)
find_package(Git REQUIRED)

use_riscv_toolchain(${KEYSTONE_BITS})

# export include directory
include_directories(AFTER ${KEYSTONE_SDK_DIR}/include)

# set paths to the libraries
set(KEYSTONE_LIB_HOST ${KEYSTONE_SDK_DIR}/lib/libkeystone-host.a)
set(KEYSTONE_LIB_EDGE ${KEYSTONE_SDK_DIR}/lib/libkeystone-edge.a)
set(KEYSTONE_LIB_VERIFIER ${KEYSTONE_SDK_DIR}/lib/libkeystone-verifier.a)
set(KEYSTONE_LIB_EAPP ${KEYSTONE_SDK_DIR}/lib/libkeystone-eapp.a)

# create a phony target "examples"
add_custom_target("examples")

# Define example sets
set(BADRAM_EXAMPLES
  hello-secret
  attestation-goodram
  attestation-badram
)

set(OTHER_EXAMPLES
  hello
  hello-native
  tests
  cache-cntr
)

# Selection logic
if ("${KEYSTONE_SELECTED_EXAMPLES}" STREQUAL "badram-examples")
  set(EXAMPLES_TO_BUILD ${BADRAM_EXAMPLES})
elseif ("${KEYSTONE_SELECTED_EXAMPLES}" STREQUAL "all")
  set(EXAMPLES_TO_BUILD ${BADRAM_EXAMPLES} ${OTHER_EXAMPLES})
else()
  # Default/Fallback: build only the "rest" of the examples
  set(EXAMPLES_TO_BUILD ${OTHER_EXAMPLES})
endif()

# add all selected examples below
foreach(example ${EXAMPLES_TO_BUILD})
  if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${example}/CMakeLists.txt)
    add_subdirectory(${example})
  else()
    message(WARNING "Example '${example}' not found or missing CMakeLists.txt")
  endif()
endforeach()
