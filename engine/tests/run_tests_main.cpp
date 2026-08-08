// tests/run_tests_main.cpp — the visible test runner.
//
// Links against whatever provides create_engine(): the reference on the server,
// the participant's engine.cpp in the boilerplate and behind the editor's Run
// button. `--json` is what the Run button parses.

#include <cstdio>
#include <cstring>
#include <string>

#include "mebench/engine.h"
#include "tests/spec_tests.h"

namespace {

std::string json_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  bool as_json = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--json") == 0) as_json = true;
  }

  const auto results = mebench::tests::run_all_spec_tests([] { return create_engine(); });

  size_t passed = 0;
  for (const auto& r : results) passed += r.passed ? 1 : 0;

  if (as_json) {
    std::printf("{\"total\":%zu,\"passed\":%zu,\"tests\":[", results.size(), passed);
    for (size_t i = 0; i < results.size(); ++i) {
      const auto& r = results[i];
      std::printf("%s{\"name\":\"%s\",\"rule\":\"%s\",\"passed\":%s,\"detail\":\"%s\"}",
                  i ? "," : "", json_escape(r.name).c_str(), json_escape(r.rule).c_str(),
                  r.passed ? "true" : "false", json_escape(r.detail).c_str());
    }
    std::printf("]}\n");
  } else {
    for (const auto& r : results) {
      std::printf("%s  %-46s  [%s]\n", r.passed ? "PASS" : "FAIL", r.name.c_str(), r.rule.c_str());
      if (!r.passed) std::printf("      %s\n", r.detail.c_str());
    }
    std::printf("\n%zu/%zu passed\n", passed, results.size());
    if (passed != results.size()) {
      std::printf(
          "\nThese are the visible tests (SPEC section 4, layer 1). They teach the rules;\n"
          "they do not grade. The correctness gate is differential fuzzing on a hidden seed.\n");
    }
  }

  return passed == results.size() ? 0 : 1;
}
