// reference/ref_plugin.cpp — exposes the reference through the same entry point
// participants implement.
//
// Two uses: it lets the reference be built as a .so so the harness's dlopen
// path is exercised by the same code that loads submissions, and it lets the
// visible test runner link against the reference without any #ifdef.

#include "mebench/engine.h"
#include "reference/reference.h"

extern "C" mebench::IMatchingEngine* create_engine() {
  return mebench::reference::make_reference_engine();
}
