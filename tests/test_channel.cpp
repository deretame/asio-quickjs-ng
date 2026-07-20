#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "channel.hpp"

namespace {

using async_simple::coro::Lazy;
using async_simple::coro::syncAwait;

// ASSERT_* uses bare `return` and cannot appear inside Lazy bodies.
// Prefer EXPECT_* inside coroutines; put hard stops outside syncAwait.

TEST(Mpsc, Ready) {
  syncAwait(
    []() -> Lazy<void> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      EXPECT_TRUE(tx.send(7));
      auto out = co_await rx.recv();
      EXPECT_TRUE(out.has_value());
      EXPECT_EQ(*out, 7);
      co_return;
    }());
}

TEST(Mpsc, SuspendUntilSend) {
  syncAwait(
    []() -> Lazy<void> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      std::optional<int> out;
      bool receiver_done = false;

      auto receiver = [&]() -> Lazy<void> {
        out = co_await rx.recv();
        receiver_done = true;
        co_return;
      };

      receiver().start([&](auto &&) {});
      EXPECT_FALSE(receiver_done);
      EXPECT_TRUE(tx.send(99));
      EXPECT_TRUE(receiver_done);
      EXPECT_TRUE(out.has_value());
      EXPECT_EQ(*out, 99);
      co_return;
    }());
}

TEST(Mpsc, Order) {
  syncAwait(
    []() -> Lazy<void> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      EXPECT_TRUE(tx.send(1));
      EXPECT_TRUE(tx.send(2));
      EXPECT_TRUE(tx.send(3));
      std::vector<int> got;
      for (int i = 0; i < 3; ++i) {
        auto out = co_await rx.recv();
        EXPECT_TRUE(out.has_value());
        got.push_back(*out);
      }
      EXPECT_EQ(got, (std::vector<int>{1, 2, 3}));
      co_return;
    }());
}

TEST(Mpsc, CloseWakesReceiver) {
  syncAwait(
    []() -> Lazy<void> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      std::optional<int> out = 123;
      bool done = false;
      auto receiver = [&]() -> Lazy<void> {
        out = co_await rx.recv();
        done = true;
        co_return;
      };
      receiver().start([&](auto &&) {});
      EXPECT_FALSE(done);
      tx.close();
      EXPECT_TRUE(done);
      EXPECT_FALSE(out.has_value());
      co_return;
    }());
}

TEST(Mpsc, LastSenderDropCloses) {
  syncAwait(
    []() -> Lazy<void> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      std::optional<int> out = 1;
      bool done = false;
      auto receiver = [&]() -> Lazy<void> {
        out = co_await rx.recv();
        done = true;
        co_return;
      };
      receiver().start([&](auto &&) {});
      EXPECT_FALSE(done);
      {
        auto drop = std::move(tx);
      }
      EXPECT_TRUE(done);
      EXPECT_FALSE(out.has_value());
      co_return;
    }());
}

TEST(Mpsc, AfterClose) {
  syncAwait(
    []() -> Lazy<void> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      tx.close();
      EXPECT_FALSE(tx.send(1));
      auto out = co_await rx.recv();
      EXPECT_FALSE(out.has_value());
      co_return;
    }());
}

TEST(Mpsc, MultiSender) {
  syncAwait(
    []() -> Lazy<void> {
      auto [tx, rx] = co::mpsc::unbounded<int>();
      auto tx2 = tx;
      EXPECT_TRUE(tx.send(1));
      EXPECT_TRUE(tx.send(2));
      EXPECT_TRUE(tx2.send(3));
      EXPECT_TRUE(tx2.send(4));
      std::vector<int> got;
      for (int i = 0; i < 4; ++i) {
        auto out = co_await rx.recv();
        EXPECT_TRUE(out.has_value());
        got.push_back(*out);
      }
      EXPECT_EQ(got, (std::vector<int>{1, 2, 3, 4}));
      co_return;
    }());
}

TEST(Oneshot, Ready) {
  syncAwait(
    []() -> Lazy<void> {
      auto [tx, rx] = co::oneshot::channel<std::string>();
      EXPECT_TRUE(tx.send(std::string("hi")));
      auto out = co_await rx.recv();
      EXPECT_TRUE(out.has_value());
      EXPECT_EQ(*out, "hi");
      co_return;
    }());
}

TEST(Oneshot, SuspendUntilSend) {
  syncAwait(
    []() -> Lazy<void> {
      auto [tx, rx] = co::oneshot::channel<std::string>();
      std::optional<std::string> out;
      bool done = false;
      auto receiver = [&]() -> Lazy<void> {
        out = co_await rx.recv();
        done = true;
        co_return;
      };
      receiver().start([&](auto &&) {});
      EXPECT_FALSE(done);
      EXPECT_TRUE(tx.send(std::string("ok")));
      EXPECT_TRUE(done);
      EXPECT_TRUE(out.has_value());
      EXPECT_EQ(*out, "ok");
      co_return;
    }());
}

TEST(Oneshot, DropSender) {
  syncAwait(
    []() -> Lazy<void> {
      auto [tx, rx] = co::oneshot::channel<int>();
      std::optional<int> out = 9;
      bool done = false;
      auto receiver = [&]() -> Lazy<void> {
        out = co_await rx.recv();
        done = true;
        co_return;
      };
      receiver().start([&](auto &&) {});
      EXPECT_FALSE(done);
      {
        auto drop = std::move(tx);
      }
      EXPECT_TRUE(done);
      EXPECT_FALSE(out.has_value());
      co_return;
    }());
}

}  // namespace
