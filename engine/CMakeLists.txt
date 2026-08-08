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

# ---------------------------------------------------------------- harness

add_library(harness_lib STATIC
  harness/engine_loader.cpp
  harness/invariants.cpp
  harness/verify.cpp)
target_include_directories(harness_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(harness_lib PUBLIC mebench_headers reference PRIVATE mebench_flags ${CMAKE_DL_LIBS})

add_executable(harness harness/harness_main.cpp)
target_link_libraries(harness PRIVATE harness_lib generator mebench_flags ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------- visible tests

add_library(spec_tests STATIC tests/spec_tests.cpp)
target_include_directories(spec_tests PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spec_tests PUBLIC mebench_headers PRIVATE mebench_flags)

# Server-side runner: the same visible tests, run against the reference.
add_executable(run_tests_reference tests/run_tests_main.cpp reference/ref_plugin.cpp)
target_link_libraries(run_tests_reference PRIVATE spec_tests reference mebench_flags)

# Deliberately broken engines, used to prove the gate catches what it claims to
# (plan section 13). Never shipped to participants.
add_library(mutant_engine SHARED tests/mutants.cpp)
target_link_libraries(mutant_engine PRIVATE reference mebench_flags)

enable_testing()
add_test(NAME visible_spec_tests COMMAND run_tests_reference)
add_test(NAME generator_validates_balanced
         COMMAND gen --seed 1 --profile balanced --events 200000 --validate)
add_test(NAME generator_validates_cancel_heavy
         COMMAND gen --seed 1 --profile cancel_heavy --events 2000000 --validate)
add_test(NAME generator_validates_sweep_heavy
         COMMAND gen --seed 1 --profile sweep_heavy --events 200000 --validate)
add_test(NAME generator_validates_deep_book
         COMMAND gen --seed 1 --profile deep_book --events 200000 --validate)
add_test(NAME reference_verifies_against_itself
         COMMAND harness verify --seed 5 --profile balanced --events 100000
                 --engine $<TARGET_FILE:reference_engine>)

# Every mutation must be CAUGHT. A gate that only ever sees correct engines has
# not been tested (plan section 13).
foreach(mutant trade_price swallow_expired self_trade lying_snapshot hang)
  add_test(NAME mutant_caught_${mutant}
           COMMAND harness verify --seed 5 --profile balanced --events 50000 --wall-time 5
                   --engine $<TARGET_FILE:mutant_engine>)
  set_tests_properties(mutant_caught_${mutant} PROPERTIES
    ENVIRONMENT "MEBENCH_MUTANT=${mutant}"
    WILL_FAIL TRUE
    TIMEOUT 120)
endforeach()
