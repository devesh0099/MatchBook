cmake_minimum_required(VERSION 3.22)
project(me_boilerplate LANGUAGES CXX)

# These flags are the ones the server uses, verbatim. Building locally with
# anything else means your local numbers are measuring a different binary.
#
# -march is explicit and is NEVER native: the correctness pool and the benchmark
# node are different machines, and a binary verified on one must be the binary
# measured on the other. If your laptop cannot run x86-64-v3 (pre-2015 Intel, or
# Apple silicon), the local benchmark will not run — use the web editor's
# Submit, which is the only path that counts anyway.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MEBENCH_MARCH "x86-64-v3" CACHE STRING "Must match the server")
add_compile_options(-O2 -march=${MEBENCH_MARCH} -fno-omit-frame-pointer -Wall -Wextra)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Your engine, as a shared object the local harness can dlopen — the same shape
# the server builds.
add_library(engine SHARED src/engine.cpp)
set_target_properties(engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The same ~30 visible tests the editor's Run button executes.
add_executable(run_tests
  src/engine.cpp
  tests/spec_tests.cpp
  tests/run_tests_main.cpp)
target_include_directories(run_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

enable_testing()
add_test(NAME visible_tests COMMAND run_tests)
