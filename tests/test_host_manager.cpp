#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

// for steady_clock in concurrent tests

#include "host_manager.hpp"
#include "job.hpp"

namespace {

job::Result await_result(co::oneshot::Receiver<job::Result> rx)
{
  return async_simple::coro::syncAwait(
    [](co::oneshot::Receiver<job::Result> r
    ) -> async_simple::coro::Lazy<job::Result> {
      auto v = co_await r.recv();
      if (!v) {
        co_return job::Result::make_err("reply closed");
      }
      co_return std::move(*v);
    }(std::move(rx)));
}

}  // namespace

TEST(HostManager, SubmitStringRoundTrip)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          function echo(s) { return s; }
        )JS",
      "echo.js"));
    });

  auto h = mgr.submit(hid, "echo", job::Args::make_string("hello"));
  ASSERT_TRUE(h.has_value());
  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::OkString);
  EXPECT_EQ(r.str, "hello");

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, SubmitObjectParsed)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          function add(o) { return { sum: o.a + o.b }; }
        )JS",
      "add.js"));
    });

  auto h = mgr.submit(
    hid,
    "add",
    job::Args::make_object_json(R"({"a":2,"b":3})"));
  ASSERT_TRUE(h.has_value());
  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::OkString);
  EXPECT_EQ(r.str, R"({"sum":5})");

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, ObjectKindVsStringKind)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          function kindOf(x) {
            return typeof x;
          }
        )JS",
      "kind.js"));
    });

  {
    auto h = mgr.submit(
      hid,
      "kindOf",
      job::Args::make_object_json(R"({"a":1})"));
    ASSERT_TRUE(h.has_value());
    auto r = await_result(std::move(h->rx));
    EXPECT_EQ(r.kind, job::ResultKind::OkString);
    EXPECT_EQ(r.str, "object");
  }
  {
    auto h = mgr.submit(
      hid,
      "kindOf",
      job::Args::make_string(R"({"a":1})"));
    ASSERT_TRUE(h.has_value());
    auto r = await_result(std::move(h->rx));
    EXPECT_EQ(r.kind, job::ResultKind::OkString);
    EXPECT_EQ(r.str, "string");
  }

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, CrossThreadSubmit)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          function mul(n) { return n * 2; }
        )JS",
      "mul.js"));
    });

  std::optional<HostManager::SubmitHandle> handle;
  std::thread t([&]() {
      handle = mgr.submit(hid, "mul", job::Args::make_object_json("21"));
    });
  t.join();
  ASSERT_TRUE(handle.has_value());
  auto r = await_result(std::move(handle->rx));
  EXPECT_EQ(r.kind, job::ResultKind::OkString);
  EXPECT_EQ(r.str, "42");

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, CancelQueuedJob)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      host.register_function(
      "sleepMs",
      [](int32_t ms) -> int32_t {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return ms;
      });
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          function slow() { call("sleepMs", 150); return "slow-done"; }
          function fast() { return "fast-done"; }
        )JS",
      "cancel.js"));
    });

  auto slow = mgr.submit(hid, "slow", job::Args::make_none());
  auto fast = mgr.submit(hid, "fast", job::Args::make_none());
  ASSERT_TRUE(slow.has_value());
  ASSERT_TRUE(fast.has_value());

  EXPECT_TRUE(mgr.cancel(fast->task_id));

  auto r_slow = await_result(std::move(slow->rx));
  auto r_fast = await_result(std::move(fast->rx));

  EXPECT_EQ(r_slow.kind, job::ResultKind::OkString);
  EXPECT_EQ(r_slow.str, "slow-done");
  EXPECT_EQ(r_fast.kind, job::ResultKind::Cancelled);

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, MissingFunction)
{
  HostManager mgr;
  auto hid = mgr.create_host();
  auto h = mgr.submit(hid, "nope", job::Args::make_none());
  ASSERT_TRUE(h.has_value());
  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::Err);
  EXPECT_NE(r.str.find("not found"), std::string::npos);
  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, UnknownHostSubmitFails)
{
  HostManager mgr;
  auto h = mgr.submit("missing", "f", job::Args::make_none());
  EXPECT_FALSE(h.has_value());
}

TEST(HostManager, AsyncJsFunction)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          async function delayedAdd(o) {
            await new Promise((resolve) => setTimeout(resolve, 30));
            return { sum: o.a + o.b };
          }
        )JS",
      "async_add.js"));
    });

  auto h = mgr.submit(
    hid,
    "delayedAdd",
    job::Args::make_object_json(R"({"a":10,"b":32})"));
  ASSERT_TRUE(h.has_value());
  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::OkString);
  EXPECT_EQ(r.str, R"({"sum":42})");

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, AsyncJsReject)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          async function fail() {
            await new Promise((resolve) => setTimeout(resolve, 10));
            throw new Error("boom");
          }
        )JS",
      "async_fail.js"));
    });

  auto h = mgr.submit(hid, "fail", job::Args::make_none());
  ASSERT_TRUE(h.has_value());
  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::Err);
  EXPECT_NE(r.str.find("boom"), std::string::npos);

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, AsyncViaRegisteredCall)
{
  // JS async function that awaits C++ async registry work (call -> Promise).
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      host.register_async_function(
      "sleepAdd",
      [&host](int32_t a, int32_t b) -> async_simple::coro::Lazy<int32_t> {
        co_await host.async_sleep(std::chrono::milliseconds(20));
        co_return a + b;
      });
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          async function viaCall(o) {
            const n = await call("sleepAdd", o.a, o.b);
            return { n: n };
          }
        )JS",
      "via_call.js"));
    });

  auto h = mgr.submit(
    hid,
    "viaCall",
    job::Args::make_object_json(R"({"a":7,"b":8})"));
  ASSERT_TRUE(h.has_value());
  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::OkString);
  EXPECT_EQ(r.str, R"({"n":15})");

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, ConcurrentAsyncJobsOverlap)
{
  // Two 100ms async jobs should finish in ~100ms wall time, not ~200ms.
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          async function wait100(tag) {
            await new Promise((r) => setTimeout(r, 100));
            return tag;
          }
        )JS",
      "overlap.js"));
    });

  auto t0 = std::chrono::steady_clock::now();
  auto a = mgr.submit(hid, "wait100", job::Args::make_string("a"));
  auto b = mgr.submit(hid, "wait100", job::Args::make_string("b"));
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());

  auto ra = await_result(std::move(a->rx));
  auto rb = await_result(std::move(b->rx));
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - t0)
    .count();

  EXPECT_EQ(ra.kind, job::ResultKind::OkString);
  EXPECT_EQ(rb.kind, job::ResultKind::OkString);
  EXPECT_EQ(ra.str, "a");
  EXPECT_EQ(rb.str, "b");
  EXPECT_LT(ms, 180) << "expected concurrent IO, wall_ms=" << ms;

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, FastJobRunsWhileSlowAwaits)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          async function slow() {
            await new Promise((r) => setTimeout(r, 80));
            return "slow";
          }
          function fast() { return "fast"; }
        )JS",
      "fast_during_slow.js"));
    });

  auto slow = mgr.submit(hid, "slow", job::Args::make_none());
  auto fast = mgr.submit(hid, "fast", job::Args::make_none());
  ASSERT_TRUE(slow.has_value());
  ASSERT_TRUE(fast.has_value());

  // fast should complete well before slow's 80ms timer.
  auto t0 = std::chrono::steady_clock::now();
  auto r_fast = await_result(std::move(fast->rx));
  auto fast_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - t0)
    .count();
  EXPECT_EQ(r_fast.str, "fast");
  EXPECT_LT(fast_ms, 50) << "fast blocked behind slow, ms=" << fast_ms;

  auto r_slow = await_result(std::move(slow->rx));
  EXPECT_EQ(r_slow.str, "slow");

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, BytesRoundTrip)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          function echoBytes(u8) {
            // return a new Uint8Array copy
            return new Uint8Array(u8);
          }
        )JS",
      "bytes.js"));
    });

  std::vector<uint8_t> in{1, 2, 3, 250, 0};
  auto h = mgr.submit(hid, "echoBytes", job::Args::make_bytes(in));
  ASSERT_TRUE(h.has_value());
  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::OkBytes);
  EXPECT_EQ(r.bytes, in);

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, NoneArgs)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          function ping() { return "pong"; }
        )JS",
      "none.js"));
    });

  auto h = mgr.submit(hid, "ping", job::Args::make_none());
  ASSERT_TRUE(h.has_value());
  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::OkString);
  EXPECT_EQ(r.str, "pong");

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, SyncThrow)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          function bad() { throw new Error("sync-boom"); }
        )JS",
      "throw.js"));
    });

  auto h = mgr.submit(hid, "bad", job::Args::make_none());
  ASSERT_TRUE(h.has_value());
  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::Err);
  EXPECT_NE(r.str.find("sync-boom"), std::string::npos);

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, TwoHostsIsolated)
{
  HostManager mgr;
  auto a = mgr.create_host(
    "host-a",
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          var n = 0;
          function inc() { n += 1; return n; }
        )JS",
      "a.js"));
    });
  auto b = mgr.create_host(
    "host-b",
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          var n = 100;
          function inc() { n += 1; return n; }
        )JS",
      "b.js"));
    });
  EXPECT_EQ(a, "host-a");
  EXPECT_EQ(b, "host-b");

  auto ha = mgr.submit(a, "inc", job::Args::make_none());
  auto hb = mgr.submit(b, "inc", job::Args::make_none());
  ASSERT_TRUE(ha.has_value());
  ASSERT_TRUE(hb.has_value());
  EXPECT_EQ(await_result(std::move(ha->rx)).str, "1");
  EXPECT_EQ(await_result(std::move(hb->rx)).str, "101");

  ha = mgr.submit(a, "inc", job::Args::make_none());
  ASSERT_TRUE(ha.has_value());
  EXPECT_EQ(await_result(std::move(ha->rx)).str, "2");

  EXPECT_TRUE(mgr.destroy_host(a));
  EXPECT_TRUE(mgr.destroy_host(b));
  EXPECT_FALSE(mgr.has_host(a));
  EXPECT_FALSE(mgr.submit(a, "inc", job::Args::make_none()).has_value());
}

TEST(HostManager, CancelRunningStillCompletes)
{
  // Soft cancel: already running job is not aborted.
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          async function slow() {
            await new Promise((r) => setTimeout(r, 80));
            return "done";
          }
        )JS",
      "run_cancel.js"));
    });

  auto h = mgr.submit(hid, "slow", job::Args::make_none());
  ASSERT_TRUE(h.has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  // May return false if the job already finished and was forgotten.
  (void)mgr.cancel(h->task_id);

  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::OkString);
  EXPECT_EQ(r.str, "done");

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, ManyConcurrentJobs)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          async function work(n) {
            await new Promise((r) => setTimeout(r, 40));
            return n * 2;
          }
        )JS",
      "many.js"));
    });

  constexpr int kN = 20;
  std::vector<HostManager::SubmitHandle> handles;
  handles.reserve(kN);
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) {
    auto h = mgr.submit(
      hid,
      "work",
      job::Args::make_object_json(std::to_string(i)));
    ASSERT_TRUE(h.has_value());
    handles.push_back(std::move(*h));
  }

  for (int i = 0; i < kN; ++i) {
    auto r = await_result(std::move(handles[i].rx));
    EXPECT_EQ(r.kind, job::ResultKind::OkString);
    EXPECT_EQ(r.str, std::to_string(i * 2));
  }
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - t0)
    .count();
  // Serial would be ~800ms; concurrent should be closer to one wave.
  EXPECT_LT(ms, 400) << "wall_ms=" << ms;

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, MaxInFlightBackpressure)
{
  // max_in_flight=1: second job starts only after first finishes.
  HostManager mgr;
  HostManager::CreateOptions opts;
  opts.max_in_flight = 1;
  opts.setup = [](Host& host) {
      ASSERT_TRUE(
        host.eval_source(
        R"JS(
          var started = [];
          async function track(tag) {
            started.push(tag);
            await new Promise((r) => setTimeout(r, 50));
            return started.join(",");
          }
        )JS",
        "cap.js"));
    };
  auto hid = mgr.create_host(std::move(opts));

  auto a = mgr.submit(hid, "track", job::Args::make_string("A"));
  auto b = mgr.submit(hid, "track", job::Args::make_string("B"));
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());

  auto ra = await_result(std::move(a->rx));
  auto rb = await_result(std::move(b->rx));
  EXPECT_EQ(ra.kind, job::ResultKind::OkString);
  EXPECT_EQ(rb.kind, job::ResultKind::OkString);
  // First job only saw itself when it finished; second saw both.
  EXPECT_EQ(ra.str, "A");
  EXPECT_EQ(rb.str, "A,B");

  EXPECT_TRUE(mgr.destroy_host(hid));
}

TEST(HostManager, DestroyWithInFlightJobs)
{
  HostManager mgr;
  auto hid = mgr.create_host(
    [](Host& host) {
      ASSERT_TRUE(
      host.eval_source(
      R"JS(
          async function longJob() {
            await new Promise((r) => setTimeout(r, 50));
            return "ok";
          }
        )JS",
      "destroy.js"));
    });

  auto h = mgr.submit(hid, "longJob", job::Args::make_none());
  ASSERT_TRUE(h.has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // destroy waits for in-flight jobs to finish (timers still run until drained).
  EXPECT_TRUE(mgr.destroy_host(hid));

  auto r = await_result(std::move(h->rx));
  EXPECT_EQ(r.kind, job::ResultKind::OkString);
  EXPECT_EQ(r.str, "ok");
}
