#include "host.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "function_registry.hpp"

namespace {

std::string join_args(qjs::Args args)
{
  std::string out;
  for (int i = 0; i < args.size(); ++i) {
    if (i > 0) {
      out.push_back(' ');
    }
    auto s = args[i].to_std_string();
    if (!s) {
      throw qjs::detail::ConvertError{};
    }
    out += *s;
  }
  return out;
}

void log_args(spdlog::level::level_enum level, qjs::Args args)
{
  spdlog::log(level, "{}", join_args(args));
}

void print_fn(qjs::Args args) { log_args(spdlog::level::info, args); }

void console_debug_fn(qjs::Args args) { log_args(spdlog::level::debug, args); }

void console_info_fn(qjs::Args args) { log_args(spdlog::level::info, args); }

void console_log_fn(qjs::Args args) { log_args(spdlog::level::info, args); }

void console_warn_fn(qjs::Args args) { log_args(spdlog::level::warn, args); }

void console_error_fn(qjs::Args args) { log_args(spdlog::level::err, args); }

async_simple::coro::Lazy<void> timeout_coro(
  Host* host,
  qjs::Value callback,
  std::chrono::milliseconds delay
)
{
  co_await host->async_sleep(delay);
  if (!host->stopping) {
    qjs::Value ret = callback.call();
    if (ret.is_exception()) {
      host->ctx.dump_exception();
    }
    host->drain_jobs();
  }
  --host->pending_ops;
  co_return;
}

void set_timeout_fn(
  Host* host,
  qjs::Value callback,
  std::optional<int32_t> delay_ms
)
{
  if (!callback.is_function()) {
    host->throw_type_error("setTimeout(fn, ms)");
  }
  int32_t ms = delay_ms.value_or(0);
  if (ms < 0) {
    ms = 0;
  }
  ++host->pending_ops;
  host->spawn_lazy(
    timeout_coro(host, std::move(callback), std::chrono::milliseconds(ms)));
}

}  // namespace

Host::Host()
{
  boost::uuids::random_generator gen;
  host_id = boost::uuids::to_string(gen());
}

Host::Host(std::string id) : host_id(std::move(id)) {}

Host::~Host() { shutdown(); }

void Host::spdlog_lazy_error(const char* msg)
{
  spdlog::error("lazy exception: {}", msg);
}

void Host::register_function(const std::string& name, SyncFunction fn)
{
  registry.register_function(name, std::move(fn));
}

void Host::register_async_function(const std::string& name, AsyncFunction fn)
{
  registry.register_async_function(name, std::move(fn));
}

void Host::register_global_function(const std::string& name, SyncFunction fn)
{
  FunctionRegistry::register_global_function(name, std::move(fn));
}

void Host::register_global_async_function(
  const std::string& name,
  AsyncFunction fn
)
{
  FunctionRegistry::register_global_async_function(name, std::move(fn));
}

void Host::shutdown() { stopping = true; }

void Host::throw_type_error(const char* msg)
{
  JS_ThrowTypeError(ctx.get(), "%s", msg);
  throw std::runtime_error(msg);
}

void Host::throw_internal_error(const char* msg)
{
  JS_ThrowInternalError(ctx.get(), "%s", msg);
  throw std::runtime_error(msg);
}

async_simple::coro::Lazy<void> Host::async_sleep(std::chrono::milliseconds ms)
{
  async_simple::Promise<void> p;
  auto fut = p.getFuture();
  auto timer = std::make_shared<asio::steady_timer>(ioc);
  timer->expires_after(ms);
  timer->async_wait(
    [p = std::move(p), timer](const asio::error_code&) mutable {
      p.setValue();
    });
  co_await std::move(fut);
  co_return;
}

bool Host::install_runtime()
{
  ctx.set_opaque(this);
  auto g = global();
  g.fn<&print_fn>("print");
  g.fn<&set_timeout_fn>("setTimeout");
  g.obj(
    "console",
    [](qjs::Value& c) {
      c.fn<&console_log_fn>("log");
      c.fn<&console_debug_fn>("debug");
      c.fn<&console_info_fn>("info");
      c.fn<&console_warn_fn>("warn");
      c.fn<&console_error_fn>("error");
    });
  g.set("__hostID", ctx.new_string(host_id));
  g.set(
    "call",
    qjs::Value::take(
    ctx.get(),
    JS_NewCFunction(ctx.get(), &native_call, "call", 1)));
  return true;
}

bool Host::eval_source(
  std::string_view code,
  const char* filename,
  bool drain
)
{
  qjs::Value ret = ctx.eval(code, filename, JS_EVAL_TYPE_GLOBAL);
  if (ret.is_exception()) {
    ctx.dump_exception();
    return false;
  }
  if (drain) {
    drain_jobs();
  }
  return true;
}

bool Host::eval_file(const char* path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    spdlog::error("failed to open {}: {}", path, std::strerror(errno));
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return eval_source(ss.str(), path);
}

void Host::run_loop()
{
  while (pending_ops > 0 || rt.job_pending()) {
    drain_jobs();
    if (pending_ops == 0 && !rt.job_pending()) {
      break;
    }
    if (ioc.stopped()) {
      ioc.restart();
    }
    ioc.run_one();
  }
  drain_jobs();
}
