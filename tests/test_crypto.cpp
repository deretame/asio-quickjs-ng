#include <gtest/gtest.h>

#include <regex>
#include <string>

#include "crypto.hpp"
#include "fetch.hpp"
#include "host.hpp"
#include "qjs.hpp"

namespace {

bool setup_host(Host& host)
{
  if (!host) {
    return false;
  }
  if (!host.install_runtime()) {
    return false;
  }
  if (!crypto_api::install(host)) {
    return false;
  }
  return fetch_api::install(host);
}

std::string js_str(qjs::Value v)
{
  auto s = v.to_std_string();
  return s.value_or("");
}

}  // namespace

TEST(Crypto, Sha256Hex) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      crypto.sha256("hello").then(function (r) { globalThis.__result = r; });
    )JS",
    "sha256_hex.js"));
  EXPECT_EQ(
    js_str(host.global().get("__result")),
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST(Crypto, CreateHashSha256) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      globalThis.__result = crypto.createHash("sha256").update("hello").digest("hex");
    )JS",
    "create_hash.js"));
  EXPECT_EQ(
    js_str(host.global().get("__result")),
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST(Crypto, CreateHashSha256Buffer) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      var b = crypto.createHash("sha256").update("hello").digest();
      globalThis.__isBuffer = Buffer.isBuffer(b);
      globalThis.__result = b.toString("hex");
    )JS",
    "create_hash_buffer.js"));
  EXPECT_EQ(js_str(host.global().get("__isBuffer")), "true");
  EXPECT_EQ(
    js_str(host.global().get("__result")),
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST(Crypto, CreateHmacSha256) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      globalThis.__result = crypto.createHmac("sha256", "key").update("hello").digest("hex");
    )JS",
    "create_hmac.js"));
  EXPECT_EQ(
    js_str(host.global().get("__result")),
    "9307b3b915efb5171ff14d8cb55fbcc798c6c0ef1456d66ded1a6aa723a58b7b");
}

TEST(Crypto, Md5Hex) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      crypto.md5("hello").then(function (r) { globalThis.__result = r; });
    )JS",
    "md5_hex.js"));
  EXPECT_EQ(
    js_str(host.global().get("__result")),
    "5d41402abc4b2a76b9719d911017c592");
}

TEST(Crypto, AesCbcRoundTrip) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      var key = "0123456789abcdef";
      var iv = "abcdef0123456789";
      crypto.aesCbcPkcs7Encrypt("hello world", key, iv).then(function (enc) {
        crypto.aesCbcPkcs7Decrypt(enc, key, iv).then(function (dec) {
          globalThis.__result = dec.toString("utf8");
        });
      });
    )JS",
    "aes_cbc_roundtrip.js"));
  EXPECT_EQ(js_str(host.global().get("__result")), "hello world");
}

TEST(Crypto, AesGcmRoundTrip) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      var key = "0123456789abcdef";
      var nonce = "0123456789ab";
      crypto.aesGcmEncrypt("hello world", key, nonce).then(function (enc) {
        return crypto.aesGcmDecrypt(enc, key, nonce);
      }).then(function (dec) {
        globalThis.__result = dec.toString("utf8");
      });
    )JS",
    "aes_gcm_roundtrip.js"));
  EXPECT_EQ(js_str(host.global().get("__result")), "hello world");
}

TEST(Crypto, RandomBytesReturnsBuffer) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      var b = crypto.randomBytes(16);
      globalThis.__isBuffer = Buffer.isBuffer(b);
      globalThis.__len = b.length;
    )JS",
    "random_bytes.js"));

  EXPECT_EQ(js_str(host.global().get("__isBuffer")), "true");
  auto len = host.global().get("__len");
  int32_t len_i32 = 0;
  ASSERT_TRUE(len.to_int32(len_i32));
  EXPECT_EQ(len_i32, 16);
}

TEST(Crypto, RandomUUIDFormat) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      globalThis.__result = crypto.randomUUID();
    )JS",
    "random_uuid.js"));

  std::string uuid = js_str(host.global().get("__result"));
  EXPECT_TRUE(
    std::regex_match(
    uuid,
    std::regex(
    "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")));
}

TEST(Crypto, TimingSafeEqual) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      globalThis.__eq = crypto.timingSafeEqual("abc", "abc");
      globalThis.__neq = crypto.timingSafeEqual("abc", "def");
    )JS",
    "timing_safe_equal.js"));

  EXPECT_EQ(js_str(host.global().get("__eq")), "true");
  EXPECT_EQ(js_str(host.global().get("__neq")), "false");
}

TEST(Crypto, Pbkdf2Sync) {
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      var b = crypto.pbkdf2Sync("password", "salt", 1000, 32);
      globalThis.__isBuffer = Buffer.isBuffer(b);
      globalThis.__len = b.length;
      globalThis.__hex = b.toString("hex");
    )JS",
    "pbkdf2_sync.js"));

  EXPECT_EQ(js_str(host.global().get("__isBuffer")), "true");
  auto len = host.global().get("__len");
  int32_t len_i32 = 0;
  ASSERT_TRUE(len.to_int32(len_i32));
  EXPECT_EQ(len_i32, 32);

  EXPECT_EQ(
    js_str(host.global().get("__hex")),
    "632c2812e46d4604102ba7618e9d6d7d2f8128f6266b4a03264d2a0460b7dcb3");
}

TEST(Crypto, ZeroCopyBinaryRoundTrip) {
  // C++ crypto functions accept std::span<const uint8_t> (zero-copy view of
  // JS binary) and return std::vector<uint8_t> transferred to JS as a
  // Uint8Array. This test verifies a large payload survives without extra
  // copies corrupting the data.
  Host host;
  ASSERT_TRUE(setup_host(host));

  ASSERT_TRUE(
    host.eval_source(
    R"JS(
      var size = 64 * 1024;
      var data = new Uint8Array(size);
      for (var i = 0; i < size; ++i) data[i] = i & 0xff;
      crypto.sha256(data).then(function (hash) {
        globalThis.__result = hash;
        globalThis.__len = hash.length;
      });
    )JS",
    "zero_copy.js"));

  auto len = host.global().get("__len");
  int32_t len_i32 = 0;
  ASSERT_TRUE(len.to_int32(len_i32));
  EXPECT_EQ(len_i32, 64);

  EXPECT_EQ(
    js_str(host.global().get("__result")),
    "7daca2095d0438260fa849183dfc67faa459fdf4936e1bc91eec6b281b27e4c2");
}
