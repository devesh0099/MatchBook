cmake_minimum_required(VERSION 3.22)
project(mebench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The published contest -march. NEVER "native": the correctness pool and the
# benchmark node are different instance families, and a binary verified on one
# must be the binary measured on the other (plan section 2). x86-64-v3 is
# AVX2/BMI2/FMA, satisfied by both Skylake and Ice Lake.
set(MEBENCH_MARCH "x86-64-v3" CACHE STRING "Explicit -march= for all contest builds")

# Flags participants are told about, verbatim. The harness embeds these in the
# build fingerprint that the benchmark node checks before every ranked run.
set(MEBENCH_CONTEST_FLAGS -O2 -march=${MEBENCH_MARCH} -fno-omit-frame-pointer)

add_library(mebench_headers INTERFACE)
target_include_directories(mebench_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(mebench_headers INTERFACE cxx_std_20)

# Applied to everything we build; the dev preset overrides -O2 and adds
# sanitizers, and leaves NDEBUG unset so the reference's asserts stay live.
add_library(mebench_flags INTERFACE)
target_compile_options(mebench_flags INTERFACE ${MEBENCH_CONTEST_FLAGS} -Wall -Wextra)

# ---------------------------------------------------------------- reference

add_library(reference STATIC reference/reference.cpp)
target_include_directories(reference PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(reference PUBLIC mebench_headers PRIVATE mebench_flags)
set_target_properties(reference PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The reference behind create_engine(), so the harness's dlopen path is
# exercised by exactly the code that loads submissions.
add_library(reference_engine SHARED reference/ref_plugin.cpp)
target_link_libraries(reference_engine PRIVATE reference mebench_flags)

# ---------------------------------------------------------------- generator

add_library(generator STATIC generator/generator.cpp generator/validate.cpp)
target_include_directories(generator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(generator PUBLIC mebench_headers reference PRIVATE mebench_flags)

add_executable(gen generator/gen_main.cpp)
target_link_libraries(gen PRIVATE generator mebench_flags)

# ---------------------------------------------------------------- visible tests

add_library(spec_tests STATIC tests/spec_tests.cpp)
target_include_directories(spec_tests PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spec_tests PUBLIC mebench_headers PRIVATE mebench_flags)

# Server-side runner: the same visible tests, run against the reference.
add_executable(run_tests_reference tests/run_tests_main.cpp reference/ref_plugin.cpp)
target_link_libraries(run_tests_reference PRIVATE spec_tests reference mebench_flags)

enable_testing()
add_test(NAME visible_spec_tests COMMAND run_tests_reference)
