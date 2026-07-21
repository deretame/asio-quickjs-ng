// Base64 tests: zero-copy encode/decode for strings and binary views.
#include <gtest/gtest.h>

#include <string>
#include <vector>

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

}  // namespace

TEST(Base64, EncodeString) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(host.eval_source(
    R"JS(
      globalThis.__encoded = base64.encode("hello world");
    )JS",
    "encode_string.js"));
  EXPECT_EQ(js_str(host.global().get("__encoded")), "aGVsbG8gd29ybGQ=");
}

TEST(Base64, EncodeUint8Array) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(host.eval_source(
    R"JS(
      globalThis.__encoded = base64.encode(new Uint8Array([0x00, 0x01, 0x02, 0xff, 0xfe]));
    )JS",
    "encode_u8.js"));
  EXPECT_EQ(js_str(host.global().get("__encoded")), "AAEC//4=");
}

TEST(Base64, EncodeArrayBuffer) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(host.eval_source(
    R"JS(
      var ab = new Uint8Array([0xde, 0xad, 0xbe, 0xef]).buffer;
      globalThis.__encoded = base64.encode(ab);
    )JS",
    "encode_ab.js"));
  EXPECT_EQ(js_str(host.global().get("__encoded")), "3q2+7w==");
}

TEST(Base64, DecodeReturnsUint8Array) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(host.eval_source(
    R"JS(
      var decoded = base64.decode("aGVsbG8gd29ybGQ=");
      globalThis.__decodedType = Object.prototype.toString.call(decoded);
      globalThis.__decodedLen = decoded.length;
      globalThis.__decodedText = String.fromCharCode.apply(null, decoded);
    )JS",
    "decode.js"));
  EXPECT_EQ(js_str(host.global().get("__decodedType")), "[object Uint8Array]");
  EXPECT_EQ(js_str(host.global().get("__decodedText")), "hello world");

  auto len = host.global().get("__decodedLen");
  int32_t len_i32 = 0;
  ASSERT_TRUE(len.to_int32(len_i32));
  EXPECT_EQ(len_i32, 11);
}

TEST(Base64, RoundTripBinary) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(host.eval_source(
    R"JS(
      var original = new Uint8Array([0x00, 0x01, 0x02, 0xff, 0xfe]);
      var encoded = base64.encode(original);
      var decoded = base64.decode(encoded);
      globalThis.__encoded = encoded;
      globalThis.__decoded = decoded;
    )JS",
    "roundtrip.js"));
  EXPECT_EQ(js_str(host.global().get("__encoded")), "AAEC//4=");

  auto decoded = host.global().get("__decoded");
  auto bytes = decoded.to_bytes();
  std::vector<uint8_t> expected = {0x00, 0x01, 0x02, 0xff, 0xfe};
  EXPECT_EQ(std::vector<uint8_t>(bytes.begin(), bytes.end()), expected);
}

TEST(Base64, EncodeRejectsInvalidType) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_FALSE(host.eval_source(
    R"JS(
      base64.encode(12345);
    )JS",
    "encode_invalid.js"));
}

TEST(Base64, SpanConventionRequiresBinary) {
  // std::span<const uint8_t> binding accepts only binary views.
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(host.eval_source(
    R"JS(
      globalThis.__fn = (bytes) => {
        return call("__spanLength", bytes);
      };
    )JS",
    "span_convention_helper.js"));
  host.register_function(
    "__spanLength",
    [](std::span<const uint8_t> bytes) -> int32_t {
      return static_cast<int32_t>(bytes.size());
    });
  ASSERT_TRUE(host.eval_source(
    R"JS(
      globalThis.__spanLen = globalThis.__fn(new Uint8Array([1, 2, 3, 4]));
    )JS",
    "span_convention.js"));
  auto len = host.global().get("__spanLen");
  int32_t len_i32 = 0;
  ASSERT_TRUE(len.to_int32(len_i32));
  EXPECT_EQ(len_i32, 4);
}

TEST(Base64, VectorConventionAcceptsString) {
  // std::vector<uint8_t> binding accepts strings too (copies).
  Host host;
  ASSERT_TRUE(setup_host(host));

  host.register_function(
    "__vectorLength",
    [](std::vector<uint8_t> bytes) -> int32_t {
      return static_cast<int32_t>(bytes.size());
    });
  ASSERT_TRUE(host.eval_source(
    R"JS(
      globalThis.__vecLen = call("__vectorLength", "hello");
    )JS",
    "vector_convention.js"));
  auto len = host.global().get("__vecLen");
  int32_t len_i32 = 0;
  ASSERT_TRUE(len.to_int32(len_i32));
  EXPECT_EQ(len_i32, 5);
}

TEST(Base64, VectorConventionReturnsBinary) {
  // std::vector<uint8_t> return value becomes a Uint8Array.
  Host host;
  ASSERT_TRUE(setup_host(host));

  host.register_function(
    "__makeBytes",
    []() -> std::vector<uint8_t> {
      return {0x01, 0x02, 0x03};
    });
  ASSERT_TRUE(host.eval_source(
    R"JS(
      var b = call("__makeBytes");
      globalThis.__retType = Object.prototype.toString.call(b);
      globalThis.__retLen = b.length;
    )JS",
    "vector_return.js"));
  EXPECT_EQ(js_str(host.global().get("__retType")), "[object Uint8Array]");
  auto len = host.global().get("__retLen");
  int32_t len_i32 = 0;
  ASSERT_TRUE(len.to_int32(len_i32));
  EXPECT_EQ(len_i32, 3);
}
