#include "qjs.hpp"
#include <mutex>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "quickjs.h"
}

namespace qjs {

std::mutex Runtime::s_module_mutex;
std::unordered_map<std::string, Runtime::ModuleEntry> Runtime::s_modules;

void Runtime::register_module(
  const char* name, const char* source, size_t len)
{
  auto entry = std::make_unique<char[]>(len + 1);
  std::memcpy(entry.get(), source, len);
  entry[len] = '\0';

  std::lock_guard<std::mutex> lock(s_module_mutex);
  ModuleEntry e{std::move(entry), len};
  s_modules[name] = std::move(e);
}

JSModuleDef* Runtime::module_loader(
  JSContext* ctx, const char* module_name, void* /*opaque*/)
{
  std::string name_copy;
  {
    std::lock_guard<std::mutex> lock(s_module_mutex);
    auto it = s_modules.find(module_name);
    if (it == s_modules.end()) {
      JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
      return nullptr;
    }
    name_copy = module_name;
  }

  // Compile the module source and return the JSModuleDef
  std::unique_ptr<char[]> source_holder;
  size_t source_len = 0;
  {
    std::lock_guard<std::mutex> lock(s_module_mutex);
    auto& entry = s_modules[name_copy];
    source_len = entry.len;
    source_holder = std::make_unique<char[]>(source_len + 1);
    std::memcpy(source_holder.get(), entry.data.get(), source_len);
    source_holder[source_len] = '\0';
  }

  // Evaluate as module - this returns a JSModuleDef when using
  // JS_EVAL_TYPE_MODULE with the module loader
  JSValue result = JS_Eval(
    ctx,
    source_holder.get(),
    source_len,
    module_name,
    JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);

  if (JS_IsException(result)) {
    return nullptr;
  }

  // result is a JSModuleDef* cast to JSValue
  return static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(result));
}

}  // namespace qjs
