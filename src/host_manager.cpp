#include "host_manager.hpp"

#include <async_simple/Promise.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "qjs.hpp"

namespace {

// Bridge JS Promise settlement -> async_simple (Host thread only for JS side).
struct JsPromiseBridge {
  async_simple::Promise<std::pair<bool, qjs::Value>> done;
};

std::mutex g_bridge_mu;
std::unordered_map<int64_t, std::shared_ptr<JsPromiseBridge>> g_bridges;
std::atomic<int64_t> g_bridge_next{1};

JSValue bridge_on_settle(
  JSContext* ctx,
  JSValueConst /*this_val*/,
  int argc,
  JSValueConst* argv,
  int magic,
  JSValueConst* func_data
)
{
  int64_t id = 0;
  if (JS_ToInt64(ctx, &id, func_data[0]) < 0) {
    return JS_EXCEPTION;
  }
  std::shared_ptr<JsPromiseBridge> bridge;
  {
    std::lock_guard lock(g_bridge_mu);
    auto it = g_bridges.find(id);
    if (it == g_bridges.end()) {
      return JS_UNDEFINED;
    }
    bridge = std::move(it->second);
    g_bridges.erase(it);
  }
  const bool ok = (magic == 0);
  qjs::Value val =
    (argc > 0) ? qjs::Value::dup(ctx, argv[0]) : qjs::Value::undefined();
  bridge->done.setValue(std::make_pair(ok, std::move(val)));
  return JS_UNDEFINED;
}

async_simple::coro::Lazy<qjs::Value> await_js_value(Host* host, qjs::Value v)
{
  JSContext* ctx = host->js_raw();
  host->drain_jobs();

  JSPromiseStateEnum st = JS_PromiseState(ctx, v.raw());
  if (st == JS_PROMISE_NOT_A_PROMISE) {
    co_return v;
  }
  if (st == JS_PROMISE_FULFILLED) {
    co_return qjs::Value::take(ctx, JS_PromiseResult(ctx, v.raw()));
  }
  if (st == JS_PROMISE_REJECTED) {
    qjs::Value reason = qjs::Value::take(ctx, JS_PromiseResult(ctx, v.raw()));
    auto msg = reason.to_std_string();
    throw std::runtime_error(msg.value_or("promise rejected"));
  }

  // PENDING: promise.then(onFulfilled, onRejected)
  auto bridge = std::make_shared<JsPromiseBridge>();
  auto fut = bridge->done.getFuture();
  const int64_t id = g_bridge_next.fetch_add(1);
  {
    std::lock_guard lock(g_bridge_mu);
    g_bridges.emplace(id, bridge);
  }

  JSValue id_val = JS_NewInt64(ctx, id);
  JSValue data[1] = {id_val};
  qjs::Value on_ok = qjs::Value::take(
    ctx,
    JS_NewCFunctionData(ctx, &bridge_on_settle, 1, 0, 1, data));
  qjs::Value on_err = qjs::Value::take(
    ctx,
    JS_NewCFunctionData(ctx, &bridge_on_settle, 1, 1, 1, data));
  JS_FreeValue(ctx, id_val);

  qjs::Value then_fn = v.get("then");
  if (!then_fn.is_function()) {
    std::lock_guard lock(g_bridge_mu);
    g_bridges.erase(id);
    throw std::runtime_error("then is not a function on promise");
  }
  qjs::Value attach = then_fn.call_on(v, on_ok, on_err);
  if (attach.is_exception()) {
    std::lock_guard lock(g_bridge_mu);
    g_bridges.erase(id);
    host->ctx.dump_exception();
    throw std::runtime_error("promise.then failed");
  }
  host->drain_jobs();

  auto outcome = co_await std::move(fut);
  if (!outcome.first) {
    auto msg = outcome.second.to_std_string();
    throw std::runtime_error(msg.value_or("promise rejected"));
  }
  co_return std::move(outcome.second);
}

qjs::Value decode_args(Host* host, const job::Args& args)
{
  JSContext* ctx = host->js_raw();
  switch (args.kind) {
  case job::ArgsKind::None:
    return qjs::Value::undefined();

  case job::ArgsKind::String:
    return host->ctx.new_string(args.str);

  case job::ArgsKind::Object: {
    JSValue parsed = JS_ParseJSON(
      ctx,
      args.str.data(),
      args.str.size(),
      "<job-args>");
    if (JS_IsException(parsed)) {
      host->ctx.dump_exception();
      throw std::runtime_error("job args Object: JSON.parse failed");
    }
    return qjs::Value::take(ctx, parsed);
  }
  case job::ArgsKind::Bytes: {
    std::vector<uint8_t> copy = args.bytes;
    return qjs::new_uint8_array(ctx, std::move(copy));
  }
  }
  return qjs::Value::undefined();
}

job::Result encode_result(Host* host, qjs::Value ret)
{
  JSContext* ctx = host->js_raw();
  if (ret.is_exception()) {
    qjs::Value exc = host->ctx.ref().exception();
    auto msg = exc.to_std_string();
    return job::Result::make_err(msg.value_or("js exception"));
  }

  if (ret.is_undefined() || JS_IsNull(ret.raw())) {
    return job::Result::make_ok_string({});
  }

  if (ret.is_binary_view()) {
    auto span = ret.to_bytes();
    if (span.empty() && !ret.is_array_buffer() && !ret.is_uint8_array()) {
      return job::Result::make_err("failed to read binary result");
    }
    return job::Result::make_ok_bytes(
      std::vector<uint8_t>(span.begin(), span.end()));
  }

  // JS string: return raw text (not JSON-quoted).
  if (ret.is_string()) {
    auto raw = ret.to_std_string();
    return job::Result::make_ok_string(raw.value_or(""));
  }

  // Objects / arrays / numbers / bools -> JSON text.
  if (JS_IsObject(ret.raw()) || JS_IsNumber(ret.raw()) || JS_IsBool(ret.raw())) {
    JSValue json = JS_JSONStringify(
      ctx,
      ret.raw(),
      JS_UNDEFINED,
      JS_UNDEFINED);
    if (JS_IsException(json)) {
      host->ctx.dump_exception();
      return job::Result::make_err("JSON.stringify failed");
    }
    qjs::Value jv = qjs::Value::take(ctx, json);
    auto s = jv.to_std_string();
    if (!s) {
      return job::Result::make_err("JSON.stringify produced non-string");
    }
    return job::Result::make_ok_string(std::move(*s));
  }

  auto s = ret.to_std_string();
  if (s) {
    return job::Result::make_ok_string(std::move(*s));
  }
  return job::Result::make_err("unsupported js return type");
}

void reply_job(job::Job& j, job::Result r)
{
  if (j.reply) {
    (void)j.reply.send(std::move(r));
  }
}

}  // namespace

HostManager::~HostManager()
{
  std::vector<std::string> ids;
  {
    std::lock_guard lock(mu_);
    ids.reserve(hosts_.size());
    for (auto& [id, _] : hosts_) {
      ids.push_back(id);
    }
  }
  for (const auto& id : ids) {
    destroy_host(id);
  }
}

std::string HostManager::create_host()
{
  return create_host(CreateOptions{});
}

std::string HostManager::create_host(std::function<void(Host&)> setup)
{
  CreateOptions opts;
  opts.setup = std::move(setup);
  return create_host(std::move(opts));
}

std::string HostManager::create_host(
  std::string id,
  std::function<void(Host&)> setup
)
{
  if (id.empty()) {
    throw std::invalid_argument("host id must be non-empty");
  }
  CreateOptions opts;
  opts.id = std::move(id);
  opts.setup = std::move(setup);
  return create_host(std::move(opts));
}

std::string HostManager::create_host(CreateOptions opts)
{
  if (opts.max_in_flight < 1) {
    throw std::invalid_argument("max_in_flight must be >= 1");
  }

  auto slot = std::make_unique<Slot>();
  if (opts.id.empty()) {
    slot->host = std::make_unique<Host>();
  } else {
    slot->host = std::make_unique<Host>(std::move(opts.id));
  }
  Host* host = slot->host.get();
  if (!*host) {
    throw std::runtime_error("failed to create Host");
  }
  if (!host->install_runtime()) {
    throw std::runtime_error("Host::install_runtime failed");
  }
  if (opts.setup) {
    opts.setup(*host);
  }

  slot->run = std::make_shared<HostRunState>();
  slot->run->max_in_flight = opts.max_in_flight;

  auto [tx, rx] = co::mpsc::unbounded<job::Job>();
  slot->job_tx = std::move(tx);
  start_job_worker(host, slot->run, std::move(rx));

  const std::string host_id = host->id();
  slot->thread = std::thread(&HostManager::run_host_thread, host);

  {
    std::lock_guard lock(mu_);
    if (hosts_.contains(host_id)) {
      slot->job_tx.close();
      host->shutdown();
      if (slot->thread.joinable()) {
        slot->thread.join();
      }
      throw std::runtime_error("duplicate host id: " + host_id);
    }
    hosts_.emplace(host_id, std::move(slot));
  }
  return host_id;
}

void HostManager::run_host_thread(Host* host)
{
  host->run_loop();
}

async_simple::coro::Lazy<void> HostManager::job_worker_loop(
  HostManager* mgr,
  Host* host,
  std::shared_ptr<HostRunState> run,
  co::mpsc::Receiver<job::Job> rx
)
{
  // Cooperative concurrent dispatch: spawn each job and immediately recv the
  // next. Multiple jobs may await Promises/IO on this Host thread at once.
  for (;;) {
    std::optional<job::Job> item = co_await rx.recv();
    if (!item) {
      break;
    }

    while (run->in_flight.load(std::memory_order_acquire) >=
      run->max_in_flight){
      co_await host->async_sleep(std::chrono::milliseconds(1));
    }

    const uint64_t task_id = item->task_id;
    run->in_flight.fetch_add(1, std::memory_order_acq_rel);
    ++host->pending_ops;
    host->spawn_lazy(
      run_one_job(mgr, host, run, task_id, std::move(*item)));
  }
  --host->pending_ops;
  co_return;
}

async_simple::coro::Lazy<void> HostManager::run_one_job(
  HostManager* mgr,
  Host* host,
  std::shared_ptr<HostRunState> run,
  uint64_t task_id,
  job::Job j
)
{
  co_await execute_job(host, std::move(j));
  run->in_flight.fetch_sub(1, std::memory_order_acq_rel);
  --host->pending_ops;
  mgr->forget_task(task_id);
  co_return;
}

void HostManager::start_job_worker(
  Host* host,
  std::shared_ptr<HostRunState> run,
  co::mpsc::Receiver<job::Job> rx
)
{
  // Keep run_loop alive for the lifetime of the dispatcher.
  ++host->pending_ops;
  host->spawn_lazy(
    job_worker_loop(this, host, std::move(run), std::move(rx)));
}

void HostManager::forget_task(uint64_t task_id)
{
  std::lock_guard lock(mu_);
  cancels_.erase(task_id);
}

async_simple::coro::Lazy<void> HostManager::execute_job(Host* host, job::Job j)
{
  if (j.cancelled && j.cancelled->load(std::memory_order_acquire)) {
    reply_job(j, job::Result::make_cancelled());
    co_return;
  }

  try {
    qjs::Value fn = host->global().get(j.fn_name.c_str());
    if (!fn.is_function()) {
      reply_job(
        j,
        job::Result::make_err("function not found: " + j.fn_name));
      co_return;
    }

    qjs::Value ret;
    if (j.args.kind == job::ArgsKind::None) {
      ret = fn.call();
    } else {
      qjs::Value arg = decode_args(host, j.args);
      ret = fn.call(arg);
    }
    if (ret.is_exception()) {
      reply_job(j, encode_result(host, std::move(ret)));
      co_return;
    }

    // Sync return or Promise (async function / explicit Promise).
    qjs::Value final_val = co_await await_js_value(host, std::move(ret));
    reply_job(j, encode_result(host, std::move(final_val)));
  } catch (const std::exception& e) {
    reply_job(j, job::Result::make_err(e.what()));
  } catch (...) {
    reply_job(j, job::Result::make_err("unknown error in job"));
  }
  co_return;
}

bool HostManager::destroy_host(const std::string& host_id)
{
  std::unique_ptr<Slot> slot;
  {
    std::lock_guard lock(mu_);
    auto it = hosts_.find(host_id);
    if (it == hosts_.end()) {
      return false;
    }
    slot = std::move(it->second);
    hosts_.erase(it);
  }

  Host* host = slot->host.get();

  // Stop accepting new jobs; let in-flight jobs finish (timers still run).
  slot->job_tx.close();
  slot->job_tx = {};

  constexpr auto kDrainTimeout = std::chrono::seconds(5);
  const auto deadline = std::chrono::steady_clock::now() + kDrainTimeout;
  while (slot->run &&
    slot->run->in_flight.load(std::memory_order_acquire) > 0 &&
    std::chrono::steady_clock::now() < deadline){
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Tear down: cancel timers so any leftover pending_ops can drain.
  host->shutdown();
  host->cancel_all_timers();

  if (slot->thread.joinable()) {
    slot->thread.join();
  }
  return true;
}

std::optional<HostManager::SubmitHandle> HostManager::submit(
  const std::string& host_id,
  std::string fn_name,
  job::Args args
)
{
  auto flag = std::make_shared<std::atomic_bool>(false);
  uint64_t task_id = 0;
  co::mpsc::Sender<job::Job> tx;

  {
    std::lock_guard lock(mu_);
    auto it = hosts_.find(host_id);
    if (it == hosts_.end() || !it->second->job_tx) {
      return std::nullopt;
    }
    task_id = next_task_id_++;
    cancels_[task_id] = flag;
    tx = it->second->job_tx;
  }

  auto [reply_tx, reply_rx] = co::oneshot::channel<job::Result>();
  job::Job j;
  j.task_id = task_id;
  j.fn_name = std::move(fn_name);
  j.args = std::move(args);
  j.reply = std::move(reply_tx);
  j.cancelled = std::move(flag);

  if (!tx.send(std::move(j))) {
    std::lock_guard lock(mu_);
    cancels_.erase(task_id);
    return std::nullopt;
  }

  SubmitHandle h;
  h.task_id = task_id;
  h.rx = std::move(reply_rx);
  return h;
}

bool HostManager::cancel(uint64_t task_id)
{
  std::lock_guard lock(mu_);
  auto it = cancels_.find(task_id);
  if (it == cancels_.end()) {
    return false;
  }
  it->second->store(true, std::memory_order_release);
  return true;
}

bool HostManager::has_host(const std::string& host_id) const
{
  std::lock_guard lock(mu_);
  return hosts_.contains(host_id);
}
