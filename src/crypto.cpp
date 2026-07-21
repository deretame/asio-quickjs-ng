#include "crypto.hpp"

#include "host.hpp"
#include "js_embedded.hpp"

#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace crypto_api {

namespace {

const EVP_MD* digest_md(std::string_view name)
{
  if (name == "sha1" || name == "sha-1") {
    return EVP_sha1();
  }
  if (name == "sha256" || name == "sha-256") {
    return EVP_sha256();
  }
  if (name == "sha512" || name == "sha-512") {
    return EVP_sha512();
  }
  if (name == "md5") {
    return EVP_md5();
  }
  return nullptr;
}

const EVP_CIPHER* aes_cipher(bool cbc, size_t key_len)
{
  if (cbc) {
    if (key_len == 16) {
      return EVP_aes_128_cbc();
    }
    if (key_len == 24) {
      return EVP_aes_192_cbc();
    }
    if (key_len == 32) {
      return EVP_aes_256_cbc();
    }
  } else {
    if (key_len == 16) {
      return EVP_aes_128_ecb();
    }
    if (key_len == 24) {
      return EVP_aes_192_ecb();
    }
    if (key_len == 32) {
      return EVP_aes_256_ecb();
    }
  }
  return nullptr;
}

const EVP_CIPHER* aes_gcm_cipher(size_t key_len)
{
  if (key_len == 16) {
    return EVP_aes_128_gcm();
  }
  if (key_len == 24) {
    return EVP_aes_192_gcm();
  }
  if (key_len == 32) {
    return EVP_aes_256_gcm();
  }
  return nullptr;
}

std::vector<uint8_t> pkcs7_pad(std::span<const uint8_t> data, size_t block_size)
{
  size_t pad_len = block_size - (data.size() % block_size);
  std::vector<uint8_t> out;
  out.reserve(data.size() + pad_len);
  out.insert(out.end(), data.begin(), data.end());
  out.insert(out.end(), pad_len, static_cast<uint8_t>(pad_len));
  return out;
}

std::vector<uint8_t> pkcs7_unpad(std::span<const uint8_t> data)
{
  if (data.empty()) {
    return {};
  }
  uint8_t pad_len = data[data.size() - 1];
  if (pad_len == 0 || pad_len > 16 || pad_len > data.size()) {
    throw std::runtime_error("invalid pkcs7 padding");
  }
  for (size_t i = 0; i < pad_len; ++i) {
    if (data[data.size() - 1 - i] != pad_len) {
      throw std::runtime_error("invalid pkcs7 padding");
    }
  }
  return std::vector<uint8_t>(data.begin(), data.end() - pad_len);
}

std::vector<uint8_t> evp_hash(
  std::span<const uint8_t> data,
  const EVP_MD* md
)
{
  std::vector<uint8_t> out(EVP_MAX_MD_SIZE);
  unsigned int len = 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    throw std::runtime_error("hash: failed to create context");
  }
  if (EVP_DigestInit_ex(ctx, md, nullptr) != 1 ||
    EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
    EVP_DigestFinal_ex(ctx, out.data(), &len) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("hash: failed");
  }
  EVP_MD_CTX_free(ctx);
  out.resize(len);
  return out;
}

std::vector<uint8_t> evp_hmac(
  std::span<const uint8_t> key,
  std::span<const uint8_t> data,
  const EVP_MD* md
)
{
  std::vector<uint8_t> out(EVP_MAX_MD_SIZE);
  unsigned int len = 0;
  if (HMAC(
    md,
    key.data(),
    static_cast<int>(key.size()),
    data.data(),
    data.size(),
    out.data(),
    &len) ==
    nullptr) {
    throw std::runtime_error("hmac: failed");
  }
  out.resize(len);
  return out;
}

std::vector<uint8_t> evp_cipher(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key,
  std::span<const uint8_t> iv,
  const EVP_CIPHER* cipher,
  bool encrypt
)
{
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    throw std::runtime_error("cipher: failed to create context");
  }
  if (EVP_CipherInit_ex(ctx, cipher, nullptr, nullptr, nullptr, encrypt ? 1 : 0) !=
    1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("cipher: init failed");
  }
  if (EVP_CIPHER_CTX_set_key_length(ctx, static_cast<int>(key.size())) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("cipher: invalid key length");
  }
  if (EVP_CipherInit_ex(ctx, nullptr, nullptr, key.data(), iv.data(), encrypt ? 1 : 0) !=
    1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("cipher: key/iv failed");
  }

  std::vector<uint8_t> out(input.size() + EVP_MAX_BLOCK_LENGTH);
  int len1 = 0;
  int len2 = 0;
  if (EVP_CipherUpdate(ctx, out.data(), &len1, input.data(), static_cast<int>(input.size())) !=
    1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("cipher: update failed");
  }
  if (EVP_CipherFinal_ex(ctx, out.data() + len1, &len2) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("cipher: final failed");
  }
  EVP_CIPHER_CTX_free(ctx);
  out.resize(static_cast<size_t>(len1) + static_cast<size_t>(len2));
  return out;
}

std::vector<uint8_t> aes_gcm_crypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key,
  std::span<const uint8_t> nonce,
  std::span<const uint8_t> aad,
  bool encrypt
)
{
  const EVP_CIPHER* cipher = aes_gcm_cipher(key.size());
  if (!cipher) {
    throw std::runtime_error("aes gcm: invalid key length");
  }
  if (nonce.size() < 1) {
    throw std::runtime_error("aes gcm: nonce required");
  }

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    throw std::runtime_error("aes gcm: failed to create context");
  }
  if (EVP_CipherInit_ex(ctx, cipher, nullptr, nullptr, nullptr, encrypt ? 1 : 0) !=
    1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("aes gcm: init failed");
  }
  if (EVP_CIPHER_CTX_ctrl(
    ctx,
    EVP_CTRL_GCM_SET_IVLEN,
    static_cast<int>(nonce.size()),
    nullptr) !=
    1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("aes gcm: set iv len failed");
  }
  if (EVP_CipherInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data(), encrypt ? 1 : 0) !=
    1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("aes gcm: key/nonce failed");
  }
  if (!aad.empty()) {
    int outlen = 0;
    if (EVP_CipherUpdate(ctx, nullptr, &outlen, aad.data(), static_cast<int>(aad.size())) !=
      1) {
      EVP_CIPHER_CTX_free(ctx);
      throw std::runtime_error("aes gcm: aad failed");
    }
  }

  if (encrypt) {
    std::vector<uint8_t> out(input.size() + EVP_MAX_BLOCK_LENGTH);
    int len1 = 0;
    if (EVP_CipherUpdate(ctx, out.data(), &len1, input.data(), static_cast<int>(input.size())) !=
      1) {
      EVP_CIPHER_CTX_free(ctx);
      throw std::runtime_error("aes gcm: update failed");
    }
    int len2 = 0;
    if (EVP_CipherFinal_ex(ctx, out.data() + len1, &len2) != 1) {
      EVP_CIPHER_CTX_free(ctx);
      throw std::runtime_error("aes gcm: final failed");
    }
    out.resize(static_cast<size_t>(len1) + static_cast<size_t>(len2));

    unsigned char tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
      EVP_CIPHER_CTX_free(ctx);
      throw std::runtime_error("aes gcm: get tag failed");
    }
    out.insert(out.end(), tag, tag + 16);
    EVP_CIPHER_CTX_free(ctx);
    return out;
  } else {
    if (input.size() < 16) {
      EVP_CIPHER_CTX_free(ctx);
      throw std::runtime_error("aes gcm: input too short for tag");
    }
    const uint8_t* tag = input.data() + input.size() - 16;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag)) !=
      1) {
      EVP_CIPHER_CTX_free(ctx);
      throw std::runtime_error("aes gcm: set tag failed");
    }

    size_t cipher_len = input.size() - 16;
    std::vector<uint8_t> out(cipher_len + EVP_MAX_BLOCK_LENGTH);
    int len1 = 0;
    if (EVP_CipherUpdate(ctx, out.data(), &len1, input.data(), static_cast<int>(cipher_len)) !=
      1) {
      EVP_CIPHER_CTX_free(ctx);
      throw std::runtime_error("aes gcm: update failed");
    }
    int len2 = 0;
    if (EVP_CipherFinal_ex(ctx, out.data() + len1, &len2) != 1) {
      EVP_CIPHER_CTX_free(ctx);
      throw std::runtime_error("aes gcm: final failed");
    }
    EVP_CIPHER_CTX_free(ctx);
    out.resize(static_cast<size_t>(len1) + static_cast<size_t>(len2));
    return out;
  }
}

}  // namespace

std::vector<uint8_t> hash_md5(std::span<const uint8_t> data)
{
  return evp_hash(data, EVP_md5());
}

std::vector<uint8_t> hash_sha1(std::span<const uint8_t> data)
{
  return evp_hash(data, EVP_sha1());
}

std::vector<uint8_t> hash_sha256(std::span<const uint8_t> data)
{
  return evp_hash(data, EVP_sha256());
}

std::vector<uint8_t> hash_sha512(std::span<const uint8_t> data)
{
  return evp_hash(data, EVP_sha512());
}

std::vector<uint8_t> hmac_sha1(
  std::span<const uint8_t> key,
  std::span<const uint8_t> data
)
{
  return evp_hmac(key, data, EVP_sha1());
}

std::vector<uint8_t> hmac_sha256(
  std::span<const uint8_t> key,
  std::span<const uint8_t> data
)
{
  return evp_hmac(key, data, EVP_sha256());
}

std::vector<uint8_t> hmac_sha512(
  std::span<const uint8_t> key,
  std::span<const uint8_t> data
)
{
  return evp_hmac(key, data, EVP_sha512());
}

std::vector<uint8_t> aes_ecb_encrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key
)
{
  const EVP_CIPHER* cipher = aes_cipher(false, key.size());
  if (!cipher) {
    throw std::runtime_error("aes ecb: invalid key length");
  }
  std::vector<uint8_t> padded = pkcs7_pad(input, EVP_CIPHER_block_size(cipher));
  return evp_cipher(padded, key, std::span<const uint8_t>{}, cipher, true);
}

std::vector<uint8_t> aes_ecb_decrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key
)
{
  const EVP_CIPHER* cipher = aes_cipher(false, key.size());
  if (!cipher) {
    throw std::runtime_error("aes ecb: invalid key length");
  }
  std::vector<uint8_t> decrypted =
    evp_cipher(input, key, std::span<const uint8_t>{}, cipher, false);
  return pkcs7_unpad(decrypted);
}

std::vector<uint8_t> aes_cbc_encrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key,
  std::span<const uint8_t> iv
)
{
  const EVP_CIPHER* cipher = aes_cipher(true, key.size());
  if (!cipher) {
    throw std::runtime_error("aes cbc: invalid key length");
  }
  if (iv.size() != 16) {
    throw std::runtime_error("aes cbc: iv must be 16 bytes");
  }
  std::vector<uint8_t> padded = pkcs7_pad(input, EVP_CIPHER_block_size(cipher));
  return evp_cipher(padded, key, iv, cipher, true);
}

std::vector<uint8_t> aes_cbc_decrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key,
  std::span<const uint8_t> iv
)
{
  const EVP_CIPHER* cipher = aes_cipher(true, key.size());
  if (!cipher) {
    throw std::runtime_error("aes cbc: invalid key length");
  }
  if (iv.size() != 16) {
    throw std::runtime_error("aes cbc: iv must be 16 bytes");
  }
  std::vector<uint8_t> decrypted = evp_cipher(input, key, iv, cipher, false);
  return pkcs7_unpad(decrypted);
}

std::vector<uint8_t> aes_gcm_encrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key,
  std::span<const uint8_t> nonce,
  std::span<const uint8_t> aad
)
{
  return aes_gcm_crypt(input, key, nonce, aad, true);
}

std::vector<uint8_t> aes_gcm_decrypt(
  std::span<const uint8_t> input,
  std::span<const uint8_t> key,
  std::span<const uint8_t> nonce,
  std::span<const uint8_t> aad
)
{
  return aes_gcm_crypt(input, key, nonce, aad, false);
}

std::vector<uint8_t> random_bytes(int32_t len)
{
  if (len < 0) {
    throw std::runtime_error("random_bytes: negative length");
  }
  std::vector<uint8_t> out(static_cast<size_t>(len));
  if (RAND_bytes(out.data(), len) != 1) {
    throw std::runtime_error("random_bytes: failed");
  }
  return out;
}

std::string random_uuid()
{
  std::vector<uint8_t> bytes = random_bytes(16);
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  char out[37];
  static const char hex[] = "0123456789abcdef";
  size_t p = 0;
  for (int i = 0; i < 16; ++i) {
    out[p++] = hex[bytes[i] >> 4];
    out[p++] = hex[bytes[i] & 0x0f];
    if (i == 3 || i == 5 || i == 7 || i == 9) {
      out[p++] = '-';
    }
  }
  out[p] = '\0';
  return std::string(out);
}

bool timing_safe_equal(
  std::span<const uint8_t> a,
  std::span<const uint8_t> b
)
{
  if (a.size() != b.size()) {
    return false;
  }
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::vector<uint8_t> pbkdf2_sync(
  std::span<const uint8_t> password,
  std::span<const uint8_t> salt,
  int32_t iterations,
  int32_t key_len,
  std::string digest
)
{
  const EVP_MD* md = digest_md(digest);
  if (!md) {
    throw std::runtime_error("pbkdf2: unsupported digest");
  }
  if (iterations < 1 || key_len < 1) {
    throw std::runtime_error("pbkdf2: invalid iterations or key length");
  }
  std::vector<uint8_t> out(static_cast<size_t>(key_len));
  if (PKCS5_PBKDF2_HMAC(
    reinterpret_cast<const char*>(password.data()),
    static_cast<int>(password.size()),
    salt.data(),
    static_cast<int>(salt.size()),
    iterations,
    md,
    key_len,
    out.data()) !=
    1) {
    throw std::runtime_error("pbkdf2: failed");
  }
  return out;
}

bool install(Host& host)
{
  qjs::Value crypto = host.ctx.object();
  crypto.fn<&hash_md5>("md5");
  crypto.fn<&hash_sha1>("sha1");
  crypto.fn<&hash_sha256>("sha256");
  crypto.fn<&hash_sha512>("sha512");
  crypto.fn<&hmac_sha1>("hmacSha1");
  crypto.fn<&hmac_sha256>("hmacSha256");
  crypto.fn<&hmac_sha512>("hmacSha512");
  crypto.fn<&aes_ecb_encrypt>("aesEcbEncrypt");
  crypto.fn<&aes_ecb_decrypt>("aesEcbDecrypt");
  crypto.fn<&aes_cbc_encrypt>("aesCbcEncrypt");
  crypto.fn<&aes_cbc_decrypt>("aesCbcDecrypt");
  crypto.fn<&aes_gcm_encrypt>("aesGcmEncrypt");
  crypto.fn<&aes_gcm_decrypt>("aesGcmDecrypt");
  crypto.fn<&random_bytes>("randomBytes");
  crypto.fn<&random_uuid>("randomUUID");
  crypto.fn<&timing_safe_equal>("timingSafeEqual");
  crypto.fn<&pbkdf2_sync>("pbkdf2Sync");
  host.global().set("__nativeCrypto", std::move(crypto));

  constexpr Host::EmbeddedJs k_crypto_js[] = {
    {"js/crypto.js", js_embedded::kJsCryptoBytes, sizeof(js_embedded::kJsCryptoBytes)},
  };
  return host.install_bootstrap_js(k_crypto_js);
}

}  // namespace crypto_api
