// Global binaryStore put/take and TTL GC tests.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "binary_store.hpp"
#include "host.hpp"
#include "qjs.hpp"

namespace {

bool setup_host(Host& host)
{
  if (!host) {
    return false;
  }
  return host.install_runtime();
}

std::string js_str(qjs::Value v)
{
  auto s = v.to_std_string();
  return s.value_or("");
}

std::vector<uint8_t> js_u8(qjs::Value v)
{
  auto bytes = v.to_bytes();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

struct StoreGuard {
  StoreGuard()
  {
    binary_store::clear();
    binary_store::set_ttl(std::chrono::minutes(15));
  }

  ~StoreGuard()
  {
    binary_store::clear();
    binary_store::set_ttl(std::chrono::minutes(15));
  }

};

}  // namespace

TEST(BinaryStore, PutTakeRoundtrip) {
  StoreGuard guard;
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      var id = binaryStore.put(new Uint8Array([1, 2, 3, 250]));
      globalThis.__id = id;
      globalThis.__got = binaryStore.take(id);
      globalThis.__gotType = Object.prototype.toString.call(globalThis.__got);
      globalThis.__again = binaryStore.take(id);
    )JS",
    "put_take.js"));

  EXPECT_FALSE(js_str(host.global().get("__id")).empty());
  EXPECT_EQ(js_str(host.global().get("__gotType")), "[object Uint8Array]");
  EXPECT_EQ(js_u8(host.global().get("__got")), (std::vector<uint8_t>{1, 2, 3, 250}));
  EXPECT_TRUE(JS_IsNull(host.global().get("__again").raw()));
}

TEST(BinaryStore, TakeMissingReturnsNull) {
  StoreGuard guard;
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      globalThis.__missing = binaryStore.take("no-such-id");
    )JS",
    "take_missing.js"));
  EXPECT_TRUE(JS_IsNull(host.global().get("__missing").raw()));
}

TEST(BinaryStore, SharedAcrossHosts) {
  StoreGuard guard;
  Host a;
  Host b;
  ASSERT_TRUE(setup_host(a));
  ASSERT_TRUE(setup_host(b));

  ASSERT_TRUE(
    a.eval_source(
    R"JS(
      globalThis.__id = binaryStore.put(new Uint8Array([9, 8, 7]));
    )JS",
    "put_a.js"));
  std::string id = js_str(a.global().get("__id"));
  ASSERT_FALSE(id.empty());

  std::string script =
    "globalThis.__got = binaryStore.take(\"" + id + "\");";
  ASSERT_TRUE(b.eval_source(script, "take_b.js"));
  EXPECT_EQ(js_u8(b.global().get("__got")), (std::vector<uint8_t>{9, 8, 7}));
}

TEST(BinaryStore, GcExpiresUnused) {
  StoreGuard guard;
  binary_store::set_ttl(std::chrono::milliseconds(20));

  std::vector<uint8_t> bytes{0xde, 0xad};
  std::string id = binary_store::put(bytes);
  EXPECT_EQ(binary_store::size(), 1u);

  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  binary_store::gc();
  EXPECT_EQ(binary_store::size(), 0u);
  EXPECT_FALSE(binary_store::take(id).has_value());
}

TEST(BinaryStore, PutAcceptsArrayBuffer) {
  StoreGuard guard;
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      var ab = new Uint8Array([10, 20, 30]).buffer;
      var id = binaryStore.put(ab);
      globalThis.__got = binaryStore.take(id);
    )JS",
    "put_ab.js"));
  EXPECT_EQ(js_u8(host.global().get("__got")), (std::vector<uint8_t>{10, 20, 30}));
}
