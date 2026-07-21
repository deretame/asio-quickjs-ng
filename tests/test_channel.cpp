#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>
#include <async_simple/executors/SimpleExecutor.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "channel.hpp"

namespace {

using async_simple::coro::Lazy;

template <typename T>
T run_lazy(Lazy<T> lazy)
{
  return async_simple::coro::syncAwait(std::move(lazy));
}

TEST(Mpsc, Ready)
{
  auto result = []() -> Lazy<std::optional<int>> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      tx.send(7);
      co_return co_await rx.recv();
    }();
  auto v = run_lazy(std::move(result));
  EXPECT_TRUE(v.has_value());
  EXPECT_EQ(*v, 7);
}

TEST(Mpsc, SuspendUntilSend)
{
  auto [tx, rx] = co::mpsc::unbounded<int>();
  std::optional<int> out;
  bool receiver_done = false;

  auto receiver = [&]() -> Lazy<void> {
      out = co_await rx.recv();
      receiver_done = true;
      co_return;
    };

  receiver().start([&](auto&&) {});
  EXPECT_FALSE(receiver_done);
  EXPECT_TRUE(tx.send(99));
  EXPECT_TRUE(receiver_done);
  EXPECT_TRUE(out.has_value());
  EXPECT_EQ(*out, 99);
}

TEST(Mpsc, Order)
{
  auto result = []() -> Lazy<std::vector<int>> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      tx.send(1);
      tx.send(2);
      tx.send(3);
      std::vector<int> got;
      for (int i = 0; i < 3; ++i) {
        auto out = co_await rx.recv();
        EXPECT_TRUE(out.has_value());
        got.push_back(*out);
      }
      co_return got;
    }();
  auto got = run_lazy(std::move(result));
  EXPECT_EQ(got, (std::vector<int>{1, 2, 3}));
}

TEST(Mpsc, CloseWakesReceiver)
{
  auto [tx, rx] = co::mpsc::unbounded<int>();
  std::optional<int> out = 123;
  bool done = false;
  auto receiver = [&]() -> Lazy<void> {
      out = co_await rx.recv();
      done = true;
      co_return;
    };
  receiver().start([&](auto&&) {});
  EXPECT_FALSE(done);
  tx.close();
  EXPECT_TRUE(done);
  EXPECT_FALSE(out.has_value());
}

TEST(Mpsc, LastSenderDropCloses)
{
  auto [tx, rx] = co::mpsc::unbounded<int>();
  std::optional<int> out = 1;
  bool done = false;
  auto receiver = [&]() -> Lazy<void> {
      out = co_await rx.recv();
      done = true;
      co_return;
    };
  receiver().start([&](auto&&) {});
  EXPECT_FALSE(done);
  {
    auto drop = std::move(tx);
  }
  EXPECT_TRUE(done);
  EXPECT_FALSE(out.has_value());
}

TEST(Mpsc, AfterClose)
{
  auto result = []() -> Lazy<std::optional<int>> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      tx.close();
      co_return co_await rx.recv();
    }();
  auto v = run_lazy(std::move(result));
  EXPECT_FALSE(v.has_value());
}

TEST(Mpsc, MultiSender)
{
  auto result = []() -> Lazy<std::vector<int>> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      auto tx2 = tx;
      tx.send(1);
      tx.send(2);
      tx2.send(3);
      tx2.send(4);
      std::vector<int> got;
      for (int i = 0; i < 4; ++i) {
        auto out = co_await rx.recv();
        EXPECT_TRUE(out.has_value());
        got.push_back(*out);
      }
      co_return got;
    }();
  auto got = run_lazy(std::move(result));
  EXPECT_EQ(got, (std::vector<int>{1, 2, 3, 4}));
}

TEST(Mpsc, TryRecv)
{
  auto [tx, rx] = co::mpsc::unbounded<int>();
  EXPECT_FALSE(rx.try_recv().has_value());
  EXPECT_TRUE(tx.send(5));
  auto v = rx.try_recv();
  EXPECT_TRUE(v.has_value());
  EXPECT_EQ(*v, 5);
  EXPECT_FALSE(rx.try_recv().has_value());
}

TEST(Mpsc, SendAfterClose)
{
  auto [tx, rx] = co::mpsc::unbounded<int>();
  tx.close();
  EXPECT_FALSE(tx.send(1));
  EXPECT_FALSE(rx.try_recv().has_value());
}

// No executor: waiter resumes on the sender thread.
TEST(Mpsc, MultiThreadSendRecv)
{
  auto [tx, rx] = co::mpsc::unbounded<int>();
  constexpr int kN = 100;
  constexpr int kThreads = 4;
  constexpr int kTotal = kThreads * kN;

  std::promise<std::vector<int>> done;
  auto fut = done.get_future();

  auto consumer = [rx = std::move(rx)]() mutable -> Lazy<std::vector<int>> {
      std::vector<int> got;
      got.reserve(kTotal);
      for (int i = 0; i < kTotal; ++i) {
        auto out = co_await rx.recv();
        if (!out) {
          break;
        }
        got.push_back(*out);
      }
      std::sort(got.begin(), got.end());
      co_return got;
    };

  consumer().start(
    [&](async_simple::Try<std::vector<int>>&& t) {
      if (t.hasError()) {
        done.set_value({});
      } else {
        done.set_value(std::move(t.value()));
      }
    });

  std::vector<std::thread> producers;
  producers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back(
      [tx, t]() {
        for (int i = 0; i < kN; ++i) {
          (void)tx.send(t * kN + i);
        }
      });
  }
  for (auto& p : producers) {
    p.join();
  }

  auto got = fut.get();
  std::vector<int> expected(kTotal);
  std::iota(expected.begin(), expected.end(), 0);
  EXPECT_EQ(got, expected);
}

// Sanity: SimpleExecutor + via works at all.
TEST(Mpsc, SimpleExecutorSanity)
{
  async_simple::executors::SimpleExecutor ex(1);
  std::promise<int> done;
  auto fut = done.get_future();

  auto task = []() -> Lazy<int> { co_return 7; }().via(&ex);
  std::move(task).start(
    [&](async_simple::Try<int>&& t) {
      done.set_value(t.hasError() ? -1 : t.value());
    });
  EXPECT_EQ(fut.get(), 7);
}

// Data already queued; consumer only runs on executor (no cross-thread wake).
TEST(Mpsc, ReadyWithExecutor)
{
  async_simple::executors::SimpleExecutor ex(1);
  auto [tx, rx] = co::mpsc::unbounded<int>();
  ASSERT_TRUE(tx.send(42));

  std::promise<int> done;
  auto fut = done.get_future();

  auto body = [rx = std::move(rx)]() mutable -> Lazy<int> {
      auto out = co_await rx.recv();
      co_return out.value_or(-1);
    };
  body().via(&ex).start(
    [&](async_simple::Try<int>&& t) {
      done.set_value(t.hasError() ? -2 : t.value());
    });

  EXPECT_EQ(fut.get(), 42);
}

// Single cross-thread wake with executor.
TEST(Mpsc, CrossThreadWithExecutor)
{
  async_simple::executors::SimpleExecutor ex(1);
  auto [tx, rx] = co::mpsc::unbounded<int>();
  std::promise<int> done;
  auto fut = done.get_future();
  std::promise<void> suspended;
  auto suspended_fut = suspended.get_future();

  auto body =
    [rx = std::move(rx), &suspended]() mutable -> Lazy<int> {
      co_await async_simple::coro::Yield{};
      suspended.set_value();
      auto out = co_await rx.recv();
      co_return out.value_or(-1);
    };
  body().via(&ex).start(
    [&](async_simple::Try<int>&& t) {
      done.set_value(t.hasError() ? -2 : t.value());
    });

  suspended_fut.wait();
  std::thread([&]() { (void)tx.send(42); }).join();

  EXPECT_EQ(fut.get(), 42);
}

// Many producers + executor-bound consumer.
TEST(Mpsc, MultiThreadWithExecutor)
{
  async_simple::executors::SimpleExecutor ex(1);
  auto [tx, rx] = co::mpsc::unbounded<int>();
  constexpr int kN = 50;
  constexpr int kThreads = 4;
  constexpr int kTotal = kThreads * kN;

  std::promise<std::vector<int>> done;
  auto fut = done.get_future();

  auto body = [rx = std::move(rx)]() mutable -> Lazy<std::vector<int>> {
      std::vector<int> got;
      got.reserve(kTotal);
      for (int i = 0; i < kTotal; ++i) {
        auto out = co_await rx.recv();
        if (!out) {
          break;
        }
        got.push_back(*out);
      }
      std::sort(got.begin(), got.end());
      co_return got;
    };
  body().via(&ex).start(
    [&](async_simple::Try<std::vector<int>>&& t) {
      if (t.hasError()) {
        done.set_value({});
      } else {
        done.set_value(std::move(t.value()));
      }
    });

  std::vector<std::thread> producers;
  producers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back(
      [tx, t]() {
        for (int i = 0; i < kN; ++i) {
          (void)tx.send(t * kN + i);
        }
      });
  }
  for (auto& p : producers) {
    p.join();
  }

  auto got = fut.get();
  std::vector<int> expected(kTotal);
  std::iota(expected.begin(), expected.end(), 0);
  EXPECT_EQ(got, expected);
}

TEST(Mpsc, CloseFromOtherThread)
{
  async_simple::executors::SimpleExecutor ex(1);
  auto [tx, rx] = co::mpsc::unbounded<int>();
  std::optional<int> out = 123;
  std::atomic<bool> done{false};

  auto body = [&]() -> Lazy<void> {
      out = co_await rx.recv();
      done.store(true, std::memory_order_release);
      co_return;
    };
  std::promise<void> done_promise;
  auto fut = done_promise.get_future();

  body().via(&ex).start([&](auto&&) { done_promise.set_value(); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::thread closer([tx = std::move(tx)]() mutable { tx.close(); });
  closer.join();

  fut.get();

  EXPECT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_FALSE(out.has_value());
}

TEST(Oneshot, Ready)
{
  auto result = []() -> Lazy<std::optional<std::string>> {
      auto [tx, rx] = co::oneshot::channel<std::string>();
      tx.send(std::string("hi"));
      co_return co_await rx.recv();
    }();
  auto v = run_lazy(std::move(result));
  EXPECT_TRUE(v.has_value());
  EXPECT_EQ(*v, "hi");
}

TEST(Oneshot, SuspendUntilSend)
{
  auto [tx, rx] = co::oneshot::channel<std::string>();
  std::optional<std::string> out;
  bool done = false;
  auto receiver = [&]() -> Lazy<void> {
      out = co_await rx.recv();
      done = true;
      co_return;
    };
  receiver().start([&](auto&&) {});
  EXPECT_FALSE(done);
  EXPECT_TRUE(tx.send(std::string("ok")));
  EXPECT_TRUE(done);
  EXPECT_TRUE(out.has_value());
  EXPECT_EQ(*out, "ok");
}

TEST(Oneshot, DropSender)
{
  auto [tx, rx] = co::oneshot::channel<int>();
  std::optional<int> out = 9;
  bool done = false;
  auto receiver = [&]() -> Lazy<void> {
      out = co_await rx.recv();
      done = true;
      co_return;
    };
  receiver().start([&](auto&&) {});
  EXPECT_FALSE(done);
  {
    auto drop = std::move(tx);
  }
  EXPECT_TRUE(done);
  EXPECT_FALSE(out.has_value());
}

TEST(Oneshot, MultiThread)
{
  async_simple::executors::SimpleExecutor ex(1);
  auto [tx, rx] = co::oneshot::channel<std::string>();
  std::optional<std::string> out;
  std::atomic<bool> done{false};

  auto consumer = [&]() -> Lazy<void> {
      out = co_await rx.recv();
      done.store(true, std::memory_order_release);
      co_return;
    }().via(&ex);

  std::promise<void> done_promise;
  auto fut = done_promise.get_future();

  std::move(consumer).start([&](auto&&) { done_promise.set_value(); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::thread sender([tx = std::move(tx)]() mutable {
      EXPECT_TRUE(tx.send(std::string("from thread")));
    });
  sender.join();

  fut.get();

  EXPECT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_TRUE(out.has_value());
  EXPECT_EQ(*out, "from thread");
}

TEST(Oneshot, DoubleSendFails)
{
  auto [tx, rx] = co::oneshot::channel<int>();
  EXPECT_TRUE(tx.send(1));
  EXPECT_FALSE(tx);
}

}  // namespace
