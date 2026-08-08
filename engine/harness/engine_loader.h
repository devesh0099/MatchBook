// harness/engine_loader.h — obtain an engine, either the built-in reference or
// a participant's compiled submission.
//
// Submissions arrive as a .so exposing create_engine(). The reference is also
// built as a .so (reference_engine), so the dlopen path is exercised by exactly
// the code that loads submissions rather than by a special case.

#pragma once

#include <memory>
#include <string>

#include "mebench/engine.h"

namespace mebench::harness {

class EngineSource {
 public:
  // spec is either "builtin" or a path to a shared object.
  static std::unique_ptr<EngineSource> open(const std::string& spec, std::string& err);

  virtual ~EngineSource() = default;

  // A fresh engine each call. Shrinking replays prefixes many times and must
  // start from a clean book every time.
  virtual IMatchingEngine* create() const = 0;

  virtual std::string describe() const = 0;
};

}  // namespace mebench::harness
