#pragma once

#include <openssl/evp.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

struct Host;

namespace crypto_api {

bool install(Host& host);

std::vector<uint8_t> hash_md5(std::span<const uint8_t> data);
std::vector<uint8_t> hash_sha1(std::span<const uint8_t> data);
std::vector<uint8_t> hash_sha256(std::span<const uint8_t> data);
std::vector<uint8_t> hash_sha512(std::span<const uint8_t> data);

std::vector<uint8_t> hmac_sha1(
  std::span<const uint8_t> key,
  std::span<const uint8_t> data
  );
std::vector<uint8_t> hmac_sha256(
  std::span<const uint8_t> key,
  std::span<const uint8_t> data
  );
std::vector<uint8_t> hmac_sha512(
  std::span<const uint8_t> key,
  std::span<const uint8_t> data
  );

std::vector<uint8_t> aes_ecb_encrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key
  );
std::vector<uint8_t> aes_ecb_decrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key
  );

std::vector<uint8_t> aes_cbc_encrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key,
  std::span<const uint8_t> iv
  );
std::vector<uint8_t> aes_cbc_decrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key,
  std::span<const uint8_t> iv
  );

std::vector<uint8_t> aes_gcm_encrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key,
  std::span<const uint8_t> nonce,
  std::span<const uint8_t> aad
  );
std::vector<uint8_t> aes_gcm_decrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key,
  std::span<const uint8_t> nonce,
  std::span<const uint8_t> aad
  );

std::vector<uint8_t> random_bytes(int32_t len);
std::string random_uuid();
bool timing_safe_equal(std::span<const uint8_t> a, std::span<const uint8_t> b);

std::vector<uint8_t> pbkdf2_sync(
  std::span<const uint8_t> password,
  std::span<const uint8_t> salt,
  int32_t iterations,
  int32_t key_len,
  std::string digest
  );

}  // namespace crypto_api
