#include "harness/engine_loader.h"

#include <dlfcn.h>

#include "reference/reference.h"

namespace mebench::harness {
namespace {

class BuiltinSource final : public EngineSource {
 public:
  IMatchingEngine* create() const override { return reference::make_reference_engine(); }
  std::string describe() const override { return "builtin reference"; }
};

class SharedObjectSource final : public EngineSource {
 public:
  using Factory = IMatchingEngine* (*)();

  SharedObjectSource(void* handle, Factory factory, std::string path)
      : handle_(handle), factory_(factory), path_(std::move(path)) {}

  ~SharedObjectSource() override {
    // Deliberately not dlclose()d: engines returned by the factory may still be
    // alive, and unloading the code underneath a live vtable is a segfault that
    // looks like a submission bug.
  }

  IMatchingEngine* create() const override { return factory_(); }
  std::string describe() const override { return path_; }

 private:
  void* handle_;
  Factory factory_;
  std::string path_;
};

}  // namespace

std::unique_ptr<EngineSource> EngineSource::open(const std::string& spec, std::string& err) {
  if (spec == "builtin") return std::make_unique<BuiltinSource>();

  dlerror();  // clear any stale error
  void* handle = dlopen(spec.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    err = "cannot load engine " + spec + ": " + dlerror();
    return nullptr;
  }

  dlerror();
  void* sym = dlsym(handle, "create_engine");
  if (const char* e = dlerror(); e != nullptr) {
    err = "engine " + spec + " does not export create_engine: " + e;
    dlclose(handle);
    return nullptr;
  }
  if (!sym) {
    err = "engine " + spec + " exports a null create_engine";
    dlclose(handle);
    return nullptr;
  }

  auto factory = reinterpret_cast<SharedObjectSource::Factory>(sym);
  return std::make_unique<SharedObjectSource>(handle, factory, spec);
}

}  // namespace mebench::harness
