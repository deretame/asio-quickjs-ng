#pragma once

#include <concepts>
#include <coroutine>
#include <memory>
#include <optional>
#include <queue>
#include <type_traits>
#include <utility>

// Tokio-style channels for C++20 coroutines (async_simple::Lazy / plain Task).
// Single-threaded / strand. Standard Awaiter — no asio::awaitable required.
//
//   auto [tx, rx] = co::mpsc::unbounded<int>();
//   tx.send(1);
//   auto v = co_await rx.recv(); // optional<T>
//
//   auto [tx, rx] = co::oneshot::channel<int>();
//   tx.send(42);
//   auto v = co_await rx.recv();

namespace co {

template <typename T>
concept channel_value =
  std::movable<T> && !std::is_const_v<T> && !std::is_volatile_v<T>;

namespace detail {

template <channel_value T>
struct mpsc_state {
  std::queue<T> queue;
  std::coroutine_handle<> waiter{};
  int senders = 1;
  bool closed = false;

  void wake()
  {
    if (waiter) {
      auto h = std::exchange(waiter, {});
      h.resume();
    }
  }

  void close()
  {
    if (closed) {
      return;
    }
    closed = true;
    wake();
  }

};

template <channel_value T>
struct oneshot_state {
  std::optional<T> value;
  std::coroutine_handle<> waiter{};
  bool sent = false;
  bool closed = false;

  void wake()
  {
    if (waiter) {
      auto h = std::exchange(waiter, {});
      h.resume();
    }
  }

};

}  // namespace detail

// ---------------------------------------------------------------------------
// mpsc::unbounded
// ---------------------------------------------------------------------------
namespace mpsc {

template <channel_value T>
class Receiver;

template <channel_value T>
class Sender {
public:
Sender() = default;
explicit Sender(std::shared_ptr<detail::mpsc_state<T> > s)
  : state_(std::move(s)) {}

Sender(const Sender &o) : state_(o.state_)
{
  if (state_) {
    ++state_->senders;
  }
}

Sender& operator=(const Sender &o)
{
  if (this == &o) {
    return *this;
  }
  release();
  state_ = o.state_;
  if (state_) {
    ++state_->senders;
  }
  return *this;
}

Sender(Sender &&o) noexcept : state_(std::move(o.state_)) {}

Sender& operator=(Sender &&o) noexcept
{
  if (this == &o) {
    return *this;
  }
  release();
  state_ = std::move(o.state_);
  return *this;
}

~Sender() { release(); }

explicit operator bool() const {
  return static_cast<bool>(state_);
}

bool send(T value) const
{
  if (!state_ || state_->closed) {
    return false;
  }
  state_->queue.push(std::move(value));
  state_->wake();
  return true;
}

void close() const
{
  if (state_) {
    state_->close();
  }
}

bool is_closed() const { return !state_ || state_->closed; }

private:
void release()
{
  if (!state_) {
    return;
  }
  if (--state_->senders == 0) {
    state_->close();
  }
  state_.reset();
}

std::shared_ptr<detail::mpsc_state<T> > state_;
};

template <channel_value T>
class Receiver {
public:
Receiver() = default;
explicit Receiver(std::shared_ptr<detail::mpsc_state<T> > s)
  : state_(std::move(s)) {}

Receiver(const Receiver &) = delete;
Receiver& operator=(const Receiver &) = delete;

Receiver(Receiver &&o) noexcept : state_(std::move(o.state_)) {}

Receiver& operator=(Receiver &&o) noexcept
{
  if (this != &o) {
    close_rx();
    state_ = std::move(o.state_);
  }
  return *this;
}

~Receiver() { close_rx(); }

explicit operator bool() const {
  return static_cast<bool>(state_);
}

// co_await rx.recv() -> optional<T>  (nullopt if closed & empty)
auto recv()
{
  struct Awaiter {
    detail::mpsc_state<T> *st;

    bool await_ready() const { return !st->queue.empty() || st->closed; }

    void await_suspend(std::coroutine_handle<> h) { st->waiter = h; }

    std::optional<T> await_resume()
    {
      if (!st->queue.empty()) {
        T v = std::move(st->queue.front());
        st->queue.pop();
        return v;
      }
      return std::nullopt;
    }

  };
  return Awaiter{state_.get()};
}

std::optional<T> try_recv()
{
  if (!state_ || state_->queue.empty()) {
    return std::nullopt;
  }
  T v = std::move(state_->queue.front());
  state_->queue.pop();
  return v;
}

bool is_closed() const { return !state_ || state_->closed; }

private:
void close_rx()
{
  if (state_) {
    state_->close();
    state_.reset();
  }
}

std::shared_ptr<detail::mpsc_state<T> > state_;
};

template <channel_value T>
struct Pair {
  Sender<T> tx;
  Receiver<T> rx;
};

template <channel_value T>
Pair<T> unbounded()
{
  auto st = std::make_shared<detail::mpsc_state<T> >();
  return {Sender<T>{st}, Receiver<T>{st}};
}

}  // namespace mpsc

// ---------------------------------------------------------------------------
// oneshot
// ---------------------------------------------------------------------------
namespace oneshot {

template <channel_value T>
class Receiver;

template <channel_value T>
class Sender {
public:
Sender() = default;
explicit Sender(std::shared_ptr<detail::oneshot_state<T> > s)
  : state_(std::move(s)) {}

Sender(const Sender &) = delete;
Sender& operator=(const Sender &) = delete;
Sender(Sender &&o) noexcept : state_(std::move(o.state_)) {}

Sender& operator=(Sender &&o) noexcept
{
  if (this != &o) {
    close();
    state_ = std::move(o.state_);
  }
  return *this;
}

~Sender() { close(); }

explicit operator bool() const {
  return static_cast<bool>(state_);
}

bool send(T value)
{
  if (!state_ || state_->sent || state_->closed) {
    return false;
  }
  state_->value = std::move(value);
  state_->sent = true;
  state_->wake();
  state_.reset();
  return true;
}

void close()
{
  if (!state_) {
    return;
  }
  if (!state_->sent) {
    state_->closed = true;
    state_->wake();
  }
  state_.reset();
}

private:
std::shared_ptr<detail::oneshot_state<T> > state_;
};

template <channel_value T>
class Receiver {
public:
Receiver() = default;
explicit Receiver(std::shared_ptr<detail::oneshot_state<T> > s)
  : state_(std::move(s)) {}

Receiver(const Receiver &) = delete;
Receiver& operator=(const Receiver &) = delete;
Receiver(Receiver &&) noexcept = default;
Receiver& operator=(Receiver &&) noexcept = default;

explicit operator bool() const {
  return static_cast<bool>(state_);
}

auto recv()
{
  struct Awaiter {
    detail::oneshot_state<T> *st;

    bool await_ready() const { return st->sent || st->closed; }

    void await_suspend(std::coroutine_handle<> h) { st->waiter = h; }

    std::optional<T> await_resume()
    {
      if (st->value) {
        auto v = std::move(*st->value);
        st->value.reset();
        return v;
      }
      return std::nullopt;
    }

  };
  return Awaiter{state_.get()};
}

bool is_ready() const { return state_ && (state_->sent || state_->closed); }

private:
std::shared_ptr<detail::oneshot_state<T> > state_;
};

template <channel_value T>
struct Pair {
  Sender<T> tx;
  Receiver<T> rx;
};

template <channel_value T>
Pair<T> channel()
{
  auto st = std::make_shared<detail::oneshot_state<T> >();
  return {Sender<T>{st}, Receiver<T>{st}};
}

}  // namespace oneshot

}  // namespace co
