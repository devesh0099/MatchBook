cmake_minimum_required(VERSION 3.22)
project(mebench LANGUAGES C CXX)

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

# ---------------------------------------------------------------- harness

# The fingerprint the benchmark node checks before every ranked run. On mismatch
# it recompiles locally rather than measuring a binary it cannot vouch for
# (plan section 2).
set(MEBENCH_FINGERPRINT "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} | -O2 -march=${MEBENCH_MARCH} -fno-omit-frame-pointer | c++20")

add_library(harness_lib STATIC
  harness/engine_loader.cpp
  harness/invariants.cpp
  harness/verify.cpp
  harness/bench.cpp
  harness/solution.cpp
  harness/timing.cpp)
target_include_directories(harness_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(harness_lib PRIVATE MEBENCH_BUILD_FINGERPRINT="${MEBENCH_FINGERPRINT}")
target_link_libraries(harness_lib PUBLIC mebench_headers reference hdr_histogram PRIVATE mebench_flags ${CMAKE_DL_LIBS})

add_executable(harness harness/harness_main.cpp)
target_link_libraries(harness PRIVATE harness_lib generator mebench_flags ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------- generator

add_library(generator STATIC generator/generator.cpp generator/validate.cpp)
target_include_directories(generator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(generator PUBLIC mebench_headers reference harness_lib PRIVATE mebench_flags)

add_executable(gen generator/gen_main.cpp)
target_link_libraries(gen PRIVATE generator mebench_flags)

# ---------------------------------------------------------------- HdrHistogram

# Vendored rather than fetched at build time: the benchmark node is provisioned
# to have nothing running on it and no reason to reach the network, and a
# ranked measurement should not depend on GitHub being up. Pinned commit is
# recorded in third_party/hdr/PINNED_COMMIT.
add_library(hdr_histogram STATIC third_party/hdr/src/hdr_histogram.c)
target_include_directories(hdr_histogram PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hdr/include)
target_include_directories(hdr_histogram PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hdr/src)
set_target_properties(hdr_histogram PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
  target_link_libraries(hdr_histogram PUBLIC ${MATH_LIBRARY})
endif()

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

# Comparison engines. NOT shipped: they exist to measure whether the ranked
# profile separates a good data structure from a naive one — a question the
# platform cannot answer by measuring only the reference.
add_library(optimized_engine SHARED tests/engines/optimized.cpp)
target_link_libraries(optimized_engine PRIVATE mebench_headers mebench_flags)

enable_testing()
add_test(NAME visible_spec_tests COMMAND run_tests_reference)
add_test(NAME generator_validates_balanced
         COMMAND gen --seed 1 --profile balanced --events 200000 --validate)
# The ranked profile's machinery, at a depth that reaches steady state quickly.
#
# `--live-target 100` gives ~20k resting orders and ~256k events of warm-up, so
# every check is live inside 2M events. The shipped setting builds ~300k orders
# and needs ~3.9M events of warm-up before depth means anything — at which point
# the checks would silently SKIP, which is worse than not running them.
add_test(NAME generator_validates_cancel_heavy
         COMMAND gen --seed 1 --profile cancel_heavy --events 2000000 --live-target 100 --validate)

# The shipped ranked profile at its real depth, which is the thing the event
# actually runs. Registered only for optimised builds: under the dev preset's
# sanitizers this is minutes rather than seconds, and a test slow enough to be
# skipped by hand is a test that does not run.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_test(NAME generator_validates_cancel_heavy_at_depth
           COMMAND gen --seed 1 --profile cancel_heavy --events 6000000 --validate)
  set_tests_properties(generator_validates_cancel_heavy_at_depth PROPERTIES TIMEOUT 600)
endif()
add_test(NAME generator_validates_sweep_heavy
         COMMAND gen --seed 1 --profile sweep_heavy --events 200000 --validate)
add_test(NAME generator_validates_deep_book
         COMMAND gen --seed 1 --profile deep_book --events 200000 --validate)
add_test(NAME reference_verifies_against_itself
         COMMAND harness verify --seed 5 --profile balanced --events 100000
                 --engine $<TARGET_FILE:reference_engine>)

add_test(NAME bench_reference_runs
         COMMAND harness bench --seed 2026 --profile cancel_heavy --events 300000 --runs 2
                 --warmup 50000 --engine $<TARGET_FILE:reference_engine>)

# A wrong digest must fail the ranked run: verification inside the timed loop is
# what stops anyone winning by doing less work (SPEC 5.2).
add_test(NAME bench_digest_mismatch_caught
         COMMAND harness bench --seed 2026 --profile cancel_heavy --events 300000 --runs 2
                 --warmup 50000 --digest deadbeef --engine $<TARGET_FILE:reference_engine>)
set_tests_properties(bench_digest_mismatch_caught PROPERTIES WILL_FAIL TRUE)

# The M1 gate/solution/ladder machinery, end to end: solve bakes an answer key,
# bench honors deadlines and checkpoints, ladder climbs and stops at the first
# miss, a wrong engine is caught by the baked solution, and a stuck engine dies
# on the SIGALRM backstop (PLAN-measurement-redesign M1).
add_test(NAME gate_solution_ladder
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/ladder_test.sh
                 $<TARGET_FILE:gen> $<TARGET_FILE:harness>
                 $<TARGET_FILE:reference_engine> $<TARGET_FILE:mutant_engine>
                 ${CMAKE_CURRENT_BINARY_DIR}/ladder_test_work)
set_tests_properties(gate_solution_ladder PROPERTIES TIMEOUT 300)

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
