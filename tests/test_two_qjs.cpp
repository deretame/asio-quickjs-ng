// Two QuickJS instances over co::mpsc, driven by async_simple::Lazy.
// Verifies non-blocking recv while both VMs stay on async_simple.
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "channel.hpp"
#include "qjs.hpp"

namespace {

using async_simple::coro::Lazy;
using async_simple::coro::syncAwait;

struct Vm {
  const char *name = "?";
  qjs::Runtime rt;
  qjs::Context ctx{rt};
  co::mpsc::Sender<std::string> tx;
  std::vector<std::string> log;
};

void print_fn(Vm *vm, qjs::Args args)
{
  for (int i = 0; i < args.size(); ++i) {
    auto s = args[i].to_std_string();
    if (!s) {
      throw qjs::detail::ConvertError{};
    }
    vm->log.push_back(*s);
  }
}

void send_fn(Vm *vm, std::string msg)
{
  ASSERT_TRUE(vm->tx.send(std::move(msg)));
}

void install_vm(Vm &vm)
{
  vm.ctx.set_opaque(&vm);
  auto g = vm.ctx.global();
  g.fn<&print_fn>("print");
  g.fn<&send_fn>("send");
}

bool eval(Vm &vm, const char *code, const char *tag)
{
  qjs::Value ret = vm.ctx.eval(code, tag, JS_EVAL_TYPE_GLOBAL);
  if (ret.is_exception()) {
    vm.ctx.dump_exception();
    return false;
  }
  vm.rt.drain_jobs();
  return true;
}

Lazy<void> recv_loop(
  Vm *vm,
  co::mpsc::Receiver<std::string> rx,
  std::vector<std::string> *inbox
)
{
  for (;;) {
    std::optional<std::string> msg = co_await rx.recv();
    if (!msg) {
      break;
    }
    inbox->push_back(*msg);

    qjs::Value handler = vm->ctx.global().get("onMessage");
    if (handler.is_function()) {
      qjs::Value arg = vm->ctx.new_string(*msg);
      qjs::Value ret = handler.call(arg);
      if (ret.is_exception()) {
        vm->ctx.dump_exception();
      }
      vm->rt.drain_jobs();
    }
  }
  co_return;
}

}  // namespace

TEST(TwoQjs, NonBlockingCrossTalk) {
  auto ab = co::mpsc::unbounded<std::string>();
  auto ba = co::mpsc::unbounded<std::string>();

  Vm a;
  a.name = "A";
  a.tx = std::move(ab.tx);

  Vm b;
  b.name = "B";
  b.tx = std::move(ba.tx);

  ASSERT_TRUE(a.rt && a.ctx && b.rt && b.ctx);

  install_vm(a);
  install_vm(b);

  std::vector<std::string> a_inbox;
  std::vector<std::string> b_inbox;
  bool b_done = false;
  bool a_done = false;

  // Pumps run until first suspend on empty channel (non-blocking).
  recv_loop(&b, std::move(ab.rx), &b_inbox).start(
    [&](auto &&) {
      b_done = true;
    });
  recv_loop(&a, std::move(ba.rx), &a_inbox).start(
    [&](auto &&) {
      a_done = true;
    });

  ASSERT_TRUE(
    eval(
    b,
    R"JS(
    function onMessage(m) {
      print("onMessage", m);
      if (m === "ping") send("pong");
      if (m === "hello") send("welcome");
    }
    print("ready");
  )JS",
    "b.js"));

  ASSERT_TRUE(
    eval(
    a,
    R"JS(
    function onMessage(m) {
      print("onMessage", m);
    }
    print("ready");
    send("hello");
    send("ping");
    send("bye");
  )JS",
    "a.js"));

  // Drop last senders -> close channels -> wake suspended recvs.
  a.tx = {};
  b.tx = {};

  EXPECT_TRUE(a_done);
  EXPECT_TRUE(b_done);
  EXPECT_EQ(b_inbox, (std::vector<std::string>{"hello", "ping", "bye"}));
  EXPECT_EQ(a_inbox, (std::vector<std::string>{"welcome", "pong"}));
}
