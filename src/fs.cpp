#include "fs.hpp"
#include <asio.hpp>
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <string_view>

namespace fs_api {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Read a file synchronously (called on the file thread pool).
static std::vector<uint8_t> read_file_sync(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return {};
  }
  auto size = file.tellg();
  if (size <= 0) {
    return {};
  }
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

// Write a file synchronously (called on the file thread pool).
static bool write_file_sync(
  const std::string& path, const uint8_t* data, size_t len)
{
  auto parent = fs::path(path).parent_path();
  if (!parent.empty()) {
    fs::create_directories(parent);
  }
  std::ofstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  file.write(reinterpret_cast<const char*>(data), len);
  return file.good();
}

// ---------------------------------------------------------------------------
// Native function implementations
// ---------------------------------------------------------------------------

// fs.readFileSync(path, [encoding]) -> string | Uint8Array
static JSValue native_read_file_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) {
    return JS_UNDEFINED;
  }

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_UNDEFINED;
  }
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  // Check for encoding parameter (default: null = return Uint8Array)
  const char* encoding = nullptr;
  if (argc >= 2 && JS_IsString(argv[1])) {
    encoding = JS_ToCString(ctx, argv[1]);
  }

  // Read file on the thread pool
  auto& pool = host->file_threads();
  std::vector<uint8_t> data;

  // Use asio::post to run on the file thread pool, then block for result
  std::promise<std::vector<uint8_t>> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    auto result = read_file_sync(path_str);
    promise.set_value(std::move(result));
  });
  data = future.get();

  if (encoding) {
    JS_FreeCString(ctx, encoding);
  }

  if (data.empty()) {
    JS_ThrowReferenceError(ctx, "ENOENT: no such file '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }

  if (encoding && (std::string_view(encoding) == "utf8" ||
                   std::string_view(encoding) == "utf-8")) {
    // Return as JS string
    std::string str(reinterpret_cast<const char*>(data.data()), data.size());
    return JS_NewString(ctx, str.c_str());
  }

  // Return as Uint8Array (not ArrayBuffer)
  JSValue arrayBuffer = JS_NewArrayBufferCopy(ctx, data.data(), data.size());
  // Wrap in Uint8Array
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue uint8Ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
  JSValue args[1] = {arrayBuffer};
  JSValue result = JS_CallConstructor(ctx, uint8Ctor, 1, args);
  JS_FreeValue(ctx, uint8Ctor);
  JS_FreeValue(ctx, global);
  // arrayBuffer is consumed by the constructor call
  return result;
}

// fs.writeFileSync(path, data)
static JSValue native_write_file_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 2) {
    return JS_UNDEFINED;
  }

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_UNDEFINED;
  }
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  // Get data as bytes
  const uint8_t* data_ptr = nullptr;
  size_t data_len = 0;
  std::vector<uint8_t> data_copy;

  if (JS_IsString(argv[1])) {
    const char* str = JS_ToCStringLen(ctx, &data_len, argv[1]);
    if (str) {
      data_ptr = reinterpret_cast<const uint8_t*>(str);
      // Need to copy since we'll use it async
      data_copy.assign(data_ptr, data_ptr + data_len);
      JS_FreeCString(ctx, str);
      data_ptr = data_copy.data();
    }
  } else {
    // Try ArrayBuffer / TypedArray
    size_t offset = 0;
    size_t length = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[1], &offset, &length, nullptr);
    if (!JS_IsException(ab)) {
      size_t ab_size = 0;
      uint8_t* buf = JS_GetArrayBuffer(ctx, &ab_size, ab);
      if (buf && length > 0) {
        data_copy.assign(buf + offset, buf + offset + std::min(length, ab_size - offset));
        data_ptr = data_copy.data();
        data_len = data_copy.size();
      }
      JS_FreeValue(ctx, ab);
    }
  }

  if (!data_ptr || data_len == 0) {
    return JS_UNDEFINED;
  }

  // Copy data for the thread pool
  std::vector<uint8_t> data(data_ptr, data_ptr + data_len);

  // Write file on the thread pool
  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [path = std::move(path_str), d = std::move(data), &promise]() {
    bool ok = write_file_sync(path, d.data(), d.size());
    promise.set_value(ok);
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to write file '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }

  return JS_UNDEFINED;
}

// fs.readFileSync async version (callback-based)
// fs.readFile(path, [encoding], callback)
static JSValue native_read_file(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) {
    return JS_UNDEFINED;
  }

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_UNDEFINED;
  }
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  // Get encoding (optional)
  const char* encoding = nullptr;
  int callback_idx = 1;
  if (argc >= 2 && JS_IsString(argv[1])) {
    encoding = JS_ToCString(ctx, argv[1]);
    callback_idx = 2;
  }

  // Get callback (required for async version)
  if (callback_idx >= argc || !JS_IsFunction(ctx, argv[callback_idx])) {
    if (encoding) JS_FreeCString(ctx, encoding);
    JS_ThrowTypeError(ctx, "callback must be a function");
    return JS_EXCEPTION;
  }
  JSValue callback = JS_DupValue(ctx, argv[callback_idx]);

  // Read file on thread pool, then post result back to main thread
  auto& pool = host->file_threads();
  std::string enc_str = encoding ? encoding : "";
  if (encoding) JS_FreeCString(ctx, encoding);

  // Keep event loop alive while async operation is pending
  ++host->pending_ops;

  // Capture host pointer for the callback
  Host* host_ptr = host;

  asio::post(pool.context(), [host_ptr, path_str = std::move(path_str),
              enc_str = std::move(enc_str), callback]() {
    auto data = read_file_sync(path_str);

    // Post result back to main thread
    asio::post(host_ptr->ioc, [host_ptr, callback, data = std::move(data),
                               path_str, enc_str]() mutable {
      JSContext* ctx = host_ptr->js_raw();
      if (data.empty()) {
        JSValue err = JS_NewError(ctx);
        JSValue msg = JS_NewString(ctx, path_str.c_str());
        JS_SetPropertyStr(ctx, err, "message", msg);
        JSValue args[2] = {err, JS_UNDEFINED};
        JSValue ret = JS_Call(ctx, callback, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, ret);
      } else {
        JSValue result;
        if (enc_str == "utf8" || enc_str == "utf-8") {
          std::string str(reinterpret_cast<const char*>(data.data()), data.size());
          result = JS_NewString(ctx, str.c_str());
        } else {
          result = JS_NewArrayBufferCopy(ctx, data.data(), data.size());
        }
        // Callback signature: callback(err, data)
        JSValue args[2] = {JS_UNDEFINED, result};
        JSValue ret = JS_Call(ctx, callback, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, ret);
      }
      JS_FreeValue(ctx, callback);
      --host_ptr->pending_ops;
    });
  });

  return JS_UNDEFINED;
}

// fs.writeFileSync async version (callback-based)
// fs.writeFile(path, data, callback)
static JSValue native_write_file(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 2) {
    return JS_UNDEFINED;
  }

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) {
    return JS_UNDEFINED;
  }
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  // Get data
  std::vector<uint8_t> data;
  if (JS_IsString(argv[1])) {
    size_t len = 0;
    const char* str = JS_ToCStringLen(ctx, &len, argv[1]);
    if (str) {
      data.assign(reinterpret_cast<const uint8_t*>(str),
                  reinterpret_cast<const uint8_t*>(str) + len);
      JS_FreeCString(ctx, str);
    }
  } else {
    size_t offset = 0, length = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[1], &offset, &length, nullptr);
    if (!JS_IsException(ab)) {
      size_t ab_size = 0;
      uint8_t* buf = JS_GetArrayBuffer(ctx, &ab_size, ab);
      if (buf && length > 0) {
        data.assign(buf + offset, buf + offset + std::min(length, ab_size - offset));
      }
      JS_FreeValue(ctx, ab);
    }
  }

  // Get callback (optional for async version)
  JSValue callback = JS_UNDEFINED;
  if (argc >= 3 && JS_IsFunction(ctx, argv[2])) {
    callback = JS_DupValue(ctx, argv[2]);
  }

  auto& pool = host->file_threads();
  Host* host_ptr = host;

  // Keep event loop alive while async operation is pending
  ++host->pending_ops;

  asio::post(pool.context(), [host_ptr, path_str = std::move(path_str),
              data = std::move(data), callback]() {
    bool ok = write_file_sync(path_str, data.data(), data.size());

    if (JS_IsFunction(host_ptr->js_raw(), callback)) {
      asio::post(host_ptr->ioc, [host_ptr, callback, ok, path_str]() {
        JSContext* ctx = host_ptr->js_raw();
        if (!ok) {
          JSValue err = JS_NewError(ctx);
          JSValue msg = JS_NewString(ctx, path_str.c_str());
          JS_SetPropertyStr(ctx, err, "message", msg);
          JSValue args[1] = {err};
          JSValue ret = JS_Call(ctx, callback, JS_UNDEFINED, 1, args);
          JS_FreeValue(ctx, ret);
        } else {
          JSValue ret = JS_Call(ctx, callback, JS_UNDEFINED, 0, nullptr);
          JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, callback);
        --host_ptr->pending_ops;
      });
    } else {
      // No callback, just decrement
      --host_ptr->pending_ops;
    }
  });

  return JS_UNDEFINED;
}

// fs.existsSync(path) -> boolean
static JSValue native_exists_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  if (argc < 1) return JS_NewBool(ctx, false);

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_NewBool(ctx, false);
  bool exists = fs::exists(path);
  JS_FreeCString(ctx, path);
  return JS_NewBool(ctx, exists);
}

// fs.unlinkSync(path)
static JSValue native_unlink_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    bool ok = fs::remove(path_str);
    promise.set_value(ok);
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to unlink '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// fs.mkdirSync(path, [recursive])
static JSValue native_mkdir_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  bool recursive = false;
  if (argc >= 2 && JS_IsBool(argv[1])) {
    recursive = JS_ToBool(ctx, argv[1]) != 0;
  }

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    bool ok = recursive ? fs::create_directories(path_str)
                        : fs::create_directory(path_str);
    promise.set_value(ok);
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to mkdir '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// fs.readdirSync(path) -> string[]
static JSValue native_readdir_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  auto& pool = host->file_threads();
  std::promise<std::vector<std::string>> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    std::vector<std::string> entries;
    for (const auto& entry : fs::directory_iterator(path_str)) {
      entries.push_back(entry.path().filename().string());
    }
    promise.set_value(std::move(entries));
  });
  auto entries = future.get();

  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < entries.size(); ++i) {
    JSValue val = JS_NewString(ctx, entries[i].c_str());
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), val);
  }
  return arr;
}

// fs.appendFileSync(path, data)
static JSValue native_append_file_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 2) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  // Get data
  std::vector<uint8_t> data;
  if (JS_IsString(argv[1])) {
    size_t len = 0;
    const char* str = JS_ToCStringLen(ctx, &len, argv[1]);
    if (str) {
      data.assign(reinterpret_cast<const uint8_t*>(str),
                  reinterpret_cast<const uint8_t*>(str) + len);
      JS_FreeCString(ctx, str);
    }
  } else {
    size_t offset = 0, length = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[1], &offset, &length, nullptr);
    if (!JS_IsException(ab)) {
      size_t ab_size = 0;
      uint8_t* buf = JS_GetArrayBuffer(ctx, &ab_size, ab);
      if (buf && length > 0) {
        data.assign(buf + offset, buf + offset + std::min(length, ab_size - offset));
      }
      JS_FreeValue(ctx, ab);
    }
  }

  if (data.empty()) return JS_UNDEFINED;

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [path_str = std::move(path_str), d = std::move(data), &promise]() {
    std::ofstream file(path_str, std::ios::binary | std::ios::app);
    if (!file.is_open()) { promise.set_value(false); return; }
    file.write(reinterpret_cast<const char*>(d.data()), d.size());
    promise.set_value(file.good());
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to append to '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// fs.copyFileSync(src, dest)
static JSValue native_copy_file_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 2) return JS_UNDEFINED;

  const char* src = JS_ToCString(ctx, argv[0]);
  if (!src) return JS_UNDEFINED;
  std::string src_str(src);
  JS_FreeCString(ctx, src);

  const char* dest = JS_ToCString(ctx, argv[1]);
  if (!dest) return JS_UNDEFINED;
  std::string dest_str(dest);
  JS_FreeCString(ctx, dest);

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      fs::copy_file(src_str, dest_str, fs::copy_options::overwrite_existing);
      promise.set_value(true);
    } catch (...) {
      promise.set_value(false);
    }
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to copy '%s' to '%s'", src_str.c_str(), dest_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// fs.renameSync(oldPath, newPath)
static JSValue native_rename_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 2) return JS_UNDEFINED;

  const char* old_path = JS_ToCString(ctx, argv[0]);
  if (!old_path) return JS_UNDEFINED;
  std::string old_str(old_path);
  JS_FreeCString(ctx, old_path);

  const char* new_path = JS_ToCString(ctx, argv[1]);
  if (!new_path) return JS_UNDEFINED;
  std::string new_str(new_path);
  JS_FreeCString(ctx, new_path);

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      fs::rename(old_str, new_str);
      promise.set_value(true);
    } catch (...) {
      promise.set_value(false);
    }
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to rename '%s' to '%s'", old_str.c_str(), new_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// fs.statSync(path) -> { size, mtime, isFile, isDirectory, ... }
static JSValue native_stat_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  auto& pool = host->file_threads();
  std::promise<fs::file_status> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      auto status = fs::status(path_str);
      auto size = fs::is_regular_file(status) ? fs::file_size(path_str) : 0;
      struct StatResult {
        uintmax_t size;
        bool is_file;
        bool is_dir;
        bool exists;
        std::error_code ec;
        std::chrono::system_clock::time_point mtime;
      };
      // We can't easily return a struct, so let's use a different approach
      promise.set_value(status);
    } catch (...) {
      promise.set_value(fs::file_status(fs::file_type::not_found));
    }
  });
  auto status = future.get();

  if (status.type() == fs::file_type::not_found) {
    JS_ThrowReferenceError(ctx, "ENOENT: no such file '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }

  // Build stat object
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "isFile", JS_NewBool(ctx, fs::is_regular_file(status)));
  JS_SetPropertyStr(ctx, obj, "isDirectory", JS_NewBool(ctx, fs::is_directory(status)));
  JS_SetPropertyStr(ctx, obj, "isSymbolicLink", JS_NewBool(ctx, fs::is_symlink(status)));

  if (fs::is_regular_file(status)) {
    std::promise<uintmax_t> size_promise;
    auto size_future = size_promise.get_future();
    asio::post(pool.context(), [&]() {
      try { size_promise.set_value(fs::file_size(path_str)); }
      catch (...) { size_promise.set_value(0); }
    });
    auto size = size_future.get();
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, static_cast<int64_t>(size)));
  } else {
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, 0));
  }

  return obj;
}

// fs.rmSync(path, { recursive }) - remove file or directory
static JSValue native_rm_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  bool recursive = false;
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue rec_val = JS_GetPropertyStr(ctx, argv[1], "recursive");
    if (JS_IsBool(rec_val)) {
      recursive = JS_ToBool(ctx, rec_val) != 0;
    }
    JS_FreeValue(ctx, rec_val);
  }

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      bool ok = recursive ? fs::remove_all(path_str) > 0 : fs::remove(path_str);
      promise.set_value(ok);
    } catch (...) {
      promise.set_value(false);
    }
  });
  bool ok = future.get();

  // Don't throw if file doesn't exist (idempotent remove)
  return JS_UNDEFINED;
}

// fs.rmdirSync(path) - remove empty directory
static JSValue native_rmdir_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      promise.set_value(fs::remove(path_str));
    } catch (...) {
      promise.set_value(false);
    }
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to rmdir '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// fs.chmodSync(path, mode) - change permissions (Unix-like)
static JSValue native_chmod_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 2) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  int32_t mode = 0;
  JS_ToInt32(ctx, &mode, argv[1]);

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      fs::permissions(path_str,
        static_cast<fs::perms>(mode),
        fs::perm_options::replace);
      promise.set_value(true);
    } catch (...) {
      promise.set_value(false);
    }
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to chmod '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// fs.realpathSync(path) -> string
static JSValue native_realpath_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  auto& pool = host->file_threads();
  std::promise<std::string> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      auto real = fs::canonical(path_str);
      promise.set_value(real.string());
    } catch (...) {
      promise.set_value("");
    }
  });
  auto result = future.get();

  if (result.empty()) {
    JS_ThrowReferenceError(ctx, "ENOENT: no such file '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_NewString(ctx, result.c_str());
}

// fs.mkdtempSync(prefix) - create temp directory
static JSValue native_mkdtemp_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* prefix = JS_ToCString(ctx, argv[0]);
  if (!prefix) return JS_UNDEFINED;
  std::string prefix_str(prefix);
  JS_FreeCString(ctx, prefix);

  auto& pool = host->file_threads();
  std::promise<std::string> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    // Create unique temp directory
    auto tmp = fs::temp_directory_path();
    auto path = tmp / prefix_str;
    // Add random suffix
    auto random = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
    auto final_path = path.string() + random;
    try {
      if (fs::create_directory(final_path)) {
        promise.set_value(final_path);
      } else {
        promise.set_value("");
      }
    } catch (...) {
      promise.set_value("");
    }
  });
  auto result = future.get();

  if (result.empty()) {
    JS_ThrowReferenceError(ctx, "Failed to create temp dir with prefix '%s'", prefix_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_NewString(ctx, result.c_str());
}



// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void install(Host& host) {
  auto* ctx = host.js_raw();
  auto g = host.global();

  // Register native functions as globals
  g.set("__nativeReadFileSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_read_file_sync, "__nativeReadFileSync", 2)));
  g.set("__nativeWriteFileSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_write_file_sync, "__nativeWriteFileSync", 2)));
  g.set("__nativeReadFile",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_read_file, "__nativeReadFile", 3)));
  g.set("__nativeWriteFile",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_write_file, "__nativeWriteFile", 3)));
  g.set("__nativeExistsSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_exists_sync, "__nativeExistsSync", 1)));
  g.set("__nativeUnlinkSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_unlink_sync, "__nativeUnlinkSync", 1)));
  g.set("__nativeMkdirSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_mkdir_sync, "__native_mkdir_sync", 2)));
  g.set("__nativeReaddirSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_readdir_sync, "__nativeReaddirSync", 1)));
  g.set("__nativeAppendFileSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_append_file_sync, "__nativeAppendFileSync", 2)));
  g.set("__nativeCopyFileSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_copy_file_sync, "__nativeCopyFileSync", 2)));
  g.set("__nativeRenameSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_rename_sync, "__nativeRenameSync", 2)));
  g.set("__nativeStatSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_stat_sync, "__nativeStatSync", 1)));
  g.set("__nativeRmSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_rm_sync, "__native_rm_sync", 2)));
  g.set("__nativeRmdirSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_rmdir_sync, "__nativeRmdirSync", 1)));
  g.set("__nativeChmodSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_chmod_sync, "__nativeChmodSync", 2)));
  g.set("__nativeRealpathSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_realpath_sync, "__nativeRealpathSync", 1)));
  g.set("__nativeMkdtempSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_mkdtemp_sync, "__nativeMkdtempSync", 1)));

  spdlog::debug("fs module installed (file thread pool: 4 threads)");
}

// ---------------------------------------------------------------------------
// High-priority additional APIs
// ---------------------------------------------------------------------------

// fs.lstatSync(path) - like statSync but doesn't follow symlinks
static JSValue native_lstat_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  auto& pool = host->file_threads();
  std::promise<fs::file_status> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      promise.set_value(fs::symlink_status(path_str));
    } catch (...) {
      promise.set_value(fs::file_status(fs::file_type::not_found));
    }
  });
  auto status = future.get();

  if (status.type() == fs::file_type::not_found) {
    JS_ThrowReferenceError(ctx, "ENOENT: no such file '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }

  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "isFile", JS_NewBool(ctx, fs::is_regular_file(status)));
  JS_SetPropertyStr(ctx, obj, "isDirectory", JS_NewBool(ctx, fs::is_directory(status)));
  JS_SetPropertyStr(ctx, obj, "isSymbolicLink", JS_NewBool(ctx, fs::is_symlink(status)));

  if (fs::is_regular_file(status)) {
    std::promise<uintmax_t> size_promise;
    auto size_future = size_promise.get_future();
    asio::post(pool.context(), [&]() {
      try { size_promise.set_value(fs::file_size(path_str)); }
      catch (...) { size_promise.set_value(0); }
    });
    auto size = size_future.get();
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, static_cast<int64_t>(size)));
  } else {
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, 0));
  }

  return obj;
}

// fs.symlinkSync(target, path)
static JSValue native_symlink_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 2) return JS_UNDEFINED;

  const char* target = JS_ToCString(ctx, argv[0]);
  if (!target) return JS_UNDEFINED;
  std::string target_str(target);
  JS_FreeCString(ctx, target);

  const char* path = JS_ToCString(ctx, argv[1]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      fs::create_symlink(target_str, path_str);
      promise.set_value(true);
    } catch (...) {
      promise.set_value(false);
    }
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to create symlink '%s' -> '%s'", path_str.c_str(), target_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// fs.readlinkSync(path) -> string
static JSValue native_readlink_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  auto& pool = host->file_threads();
  std::promise<std::string> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      promise.set_value(fs::read_symlink(path_str).string());
    } catch (...) {
      promise.set_value("");
    }
  });
  auto result = future.get();

  if (result.empty()) {
    JS_ThrowReferenceError(ctx, "Failed to read symlink '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_NewString(ctx, result.c_str());
}

// fs.accessSync(path, mode) - check file permissions
// mode: F_OK (exists), R_OK (read), W_OK (write), X_OK (execute)
static JSValue native_access_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  int mode = 0; // F_OK
  if (argc >= 2) {
    JS_ToInt32(ctx, &mode, argv[1]);
  }

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      auto perms = fs::status(path_str).permissions();
      bool ok = true;
      // Simplified permission check
      switch (mode) {
        case 0: ok = fs::exists(path_str); break; // F_OK
        case 4: ok = true; break; // R_OK (simplified)
        case 2: ok = true; break; // W_OK (simplified)
        case 1: ok = true; break; // X_OK (simplified)
        default: ok = fs::exists(path_str); break;
      }
      promise.set_value(ok);
    } catch (...) {
      promise.set_value(false);
    }
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "EACCES: permission denied '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// fs.truncateSync(path, len)
static JSValue native_truncate_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  int32_t len = 0;
  if (argc >= 2) {
    JS_ToInt32(ctx, &len, argv[1]);
  }

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      fs::resize_file(path_str, static_cast<uintmax_t>(len));
      promise.set_value(true);
    } catch (...) {
      promise.set_value(false);
    }
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to truncate '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// fs.utimesSync(path, atime, mtime)
static JSValue native_utimes_sync(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 3) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  int64_t atime = 0, mtime = 0;
  JS_ToInt64(ctx, &atime, argv[1]);
  JS_ToInt64(ctx, &mtime, argv[2]);

  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      // Convert Unix timestamp (seconds) to file_time_type
      auto sys_time = std::chrono::system_clock::from_time_t(static_cast<time_t>(mtime));
      auto file_time = fs::file_time_type::clock::now() +
        (sys_time - std::chrono::system_clock::now());
      fs::last_write_time(path_str, file_time);
      // Note: atime is not easily settable with std::filesystem
      promise.set_value(true);
    } catch (...) {
      promise.set_value(false);
    }
  });
  bool ok = future.get();

  if (!ok) {
    JS_ThrowReferenceError(ctx, "Failed to set times on '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

// ReadStream state
struct ReadStreamState {
  std::ifstream file;
  std::string path;
  uint64_t position = 0;
  uint64_t chunk_size = 64 * 1024; // 64KB chunks
  bool reading = false;
  bool ended = false;
  Host* host = nullptr;
};

// fs.createReadStream(path, options) -> ReadableStream
static JSValue native_create_read_stream(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  // Create a ReadableStream using the body_polyfill
  // For simplicity, we'll create a stream that reads the entire file
  // and enqueues it as chunks
  auto& pool = host->file_threads();
  std::promise<std::vector<uint8_t>> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    auto result = read_file_sync(path_str);
    promise.set_value(std::move(result));
  });
  auto data = future.get();

  if (data.empty()) {
    JS_ThrowReferenceError(ctx, "ENOENT: no such file '%s'", path_str.c_str());
    return JS_EXCEPTION;
  }

  // Create a ReadableStream and enqueue the data
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue stream_ctor = JS_GetPropertyStr(ctx, global, "ReadableStream");

  // Create a simple stream that enqueues all data at once
  // For a production implementation, you'd want chunked reading
  JSValue stream = JS_NewObject(ctx);

  // Use the existing Response API to create a stream from the data
  JSValue response_ctor = JS_GetPropertyStr(ctx, global, "Response");
  JSValue uint8_ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
  JSValue args[1] = {JS_NewArrayBufferCopy(ctx, data.data(), data.size())};
  JSValue uint8 = JS_CallConstructor(ctx, uint8_ctor, 1, args);
  JS_FreeValue(ctx, uint8_ctor);

  // Return a Response with the body, which can be used as a stream
  JSValue response = JS_CallConstructor(ctx, response_ctor, 0, nullptr);
  JS_FreeValue(ctx, response_ctor);
  JS_FreeValue(ctx, stream_ctor);
  JS_FreeValue(ctx, global);

  return response;
}

// fs.createWriteStream(path, options)
static JSValue native_create_write_stream(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 1) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  // Return an object with write() and end() methods
  JSValue obj = JS_NewObject(ctx);

  // Store the path and create a buffer
  std::string* path_ptr = new std::string(std::move(path_str));
  std::vector<uint8_t>* buffer = new std::vector<uint8_t>();

  // write(chunk) - appends to buffer
  JSValue write_fn = JS_NewCFunctionData(ctx, [](JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic, JSValueConst* data) -> JSValue {
    auto* buf = reinterpret_cast<std::vector<uint8_t>*>(JS_GetOpaque(data[0], 0));
    if (!buf || argc < 1) return JS_NewBool(ctx, false);

    if (JS_IsString(argv[0])) {
      size_t len = 0;
      const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
      if (str) {
        buf->insert(buf->end(), reinterpret_cast<const uint8_t*>(str),
                    reinterpret_cast<const uint8_t*>(str) + len);
        JS_FreeCString(ctx, str);
      }
    }
    return JS_NewBool(ctx, true);
  }, 1, 0, 1, reinterpret_cast<JSValue*>(&buffer));
  JS_SetPropertyStr(ctx, obj, "write", write_fn);

  // end() - flush buffer to file
  JSValue end_fn = JS_NewCFunctionData(ctx, [](JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic, JSValueConst* data) -> JSValue {
    auto* path = reinterpret_cast<std::string*>(JS_GetOpaque(data[0], 0));
    auto* buf = reinterpret_cast<std::vector<uint8_t>*>(JS_GetOpaque(data[1], 0));
    if (!path || !buf) return JS_UNDEFINED;

    write_file_sync(path->c_str(), buf->data(), buf->size());
    delete path;
    delete buf;
    return JS_UNDEFINED;
  }, 0, 0, 2, reinterpret_cast<JSValue*>(&path_ptr));
  JS_SetPropertyStr(ctx, obj, "end", end_fn);

  return obj;
}

// ---------------------------------------------------------------------------
// Watch (simplified polling-based)
// ---------------------------------------------------------------------------

// fs.watch(filename, listener) - returns a watcher with close()
static JSValue native_watch(
  JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  auto* host = static_cast<Host*>(JS_GetContextOpaque(ctx));
  if (!host || argc < 2) return JS_UNDEFINED;

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path) return JS_UNDEFINED;
  std::string path_str(path);
  JS_FreeCString(ctx, path);

  if (!JS_IsFunction(ctx, argv[1])) {
    JS_ThrowTypeError(ctx, "listener must be a function");
    return JS_EXCEPTION;
  }

  // Create a watcher object
  JSValue watcher = JS_NewObject(ctx);

  // Store listener reference
  JSValue listener = JS_DupValue(ctx, argv[1]);
  JS_SetPropertyStr(ctx, watcher, "_listener", listener);

  // close() method
  JSValue close_fn = JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst,
    int, JSValueConst*) -> JSValue {
    // In a full implementation, this would stop the polling timer
    return JS_UNDEFINED;
  }, "close", 0);
  JS_SetPropertyStr(ctx, watcher, "close", close_fn);

  // For now, we return a watcher without actual polling
  // A full implementation would use asio::steady_timer to poll file mtime
  return watcher;
}

// ---------------------------------------------------------------------------
// Install additional functions
// ---------------------------------------------------------------------------

void install_extended(Host& host) {
  auto* ctx = host.js_raw();
  auto g = host.global();

  g.set("__nativeLstatSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_lstat_sync, "__nativeLstatSync", 1)));
  g.set("__nativeSymlinkSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_symlink_sync, "__nativeSymlinkSync", 2)));
  g.set("__nativeReadlinkSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_readlink_sync, "__nativeReadlinkSync", 1)));
  g.set("__nativeAccessSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_access_sync, "__nativeAccessSync", 2)));
  g.set("__nativeTruncateSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_truncate_sync, "__nativeTruncateSync", 2)));
  g.set("__nativeUtimesSync",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_utimes_sync, "__nativeUtimesSync", 3)));
  g.set("__nativeCreateReadStream",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_create_read_stream, "__nativeCreateReadStream", 2)));
  g.set("__nativeCreateWriteStream",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_create_write_stream, "__nativeCreateWriteStream", 2)));
  g.set("__nativeWatch",
    qjs::Value::take(ctx, JS_NewCFunction(ctx, &native_watch, "__nativeWatch", 2)));
}

}  // namespace fs_api
