// tests/spec_tests.h — the ~30 visible unit tests.
//
// These are Layer 1 (SPEC §4): teaching, not grading. They are written against
// IMatchingEngine, not against the reference, so the exact same file runs
// three places:
//
//   - server-side, against the reference (a regression suite for the oracle)
//   - in the boilerplate zip, against the participant's engine (`make test`)
//   - behind the editor's Run button, against the participant's engine
//
// Passing all of these does not mean passing the correctness gate. The gate is
// differential fuzzing on a hidden seed (Layer 2).

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "mebench/engine.h"

namespace mebench::tests {

using EngineFactory = std::function<IMatchingEngine*()>;

struct TestResult {
  std::string name;
  std::string rule;  // the SPEC rule this case pins down
  bool passed;
  std::string detail;  // populated on failure: expected vs actual
};

std::vector<TestResult> run_all_spec_tests(const EngineFactory& make_engine);

}  // namespace mebench::tests
