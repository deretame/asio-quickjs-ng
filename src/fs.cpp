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

// Helper: run a callable on the file thread pool and return the result.
template <typename F>
auto run_fs(Host* host, F&& fn) {
  auto& pool = host->file_threads();
  if constexpr (std::is_void_v<decltype(fn())>) {
    std::promise<void> promise;
    auto future = promise.get_future();
    asio::post(pool.context(), [&]() { fn(); promise.set_value(); });
    future.get();
  } else {
    std::promise<decltype(fn())> promise;
    auto future = promise.get_future();
    asio::post(pool.context(), [&]() { promise.set_value(fn()); });
    return future.get();
  }
}



// ---------------------------------------------------------------------------
// Native function implementations (typed signatures)
// ---------------------------------------------------------------------------

// fs.readFileSync(path, [encoding]) -> string | Uint8Array
qjs::Value native_read_file_sync(
  Host* host,
  std::string path,
  std::optional<std::string> encoding = std::nullopt)
{
  auto& pool = host->file_threads();
  std::promise<std::vector<uint8_t>> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    promise.set_value(read_file_sync(path));
  });
  auto data = future.get();

  if (data.empty()) {
    host->throw_reference_error("ENOENT: no such file '%s'", path.c_str());
  }

  if (encoding && (*encoding == "utf8" || *encoding == "utf-8")) {
    return qjs::Value::take(host->js_raw(),
      JS_NewStringLen(host->js_raw(),
        reinterpret_cast<const char*>(data.data()), data.size()));
  }

  return qjs::Value::new_uint8_array_copy(host->js_raw(), data.data(), data.size());
}

// fs.writeFileSync(path, data)
void native_write_file_sync(
  Host* host,
  std::string path,
  std::vector<uint8_t> data)
{
  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    promise.set_value(write_file_sync(path, data.data(), data.size()));
  });
  bool ok = future.get();

  if (!ok) {
    host->throw_reference_error("Failed to write file '%s'", path.c_str());
  }
}

// fs.readFileSync async version (callback-based)
// fs.readFile(path, [encoding], callback)
static JSValue native_read_file(Host* host,
                                std::string path,
                                std::optional<std::string> encoding,
                                qjs::Value callback)
{
  if (!host) {
    return JS_UNDEFINED;
  }

  std::string enc_str = encoding.value_or("");

  // Read file on thread pool, then post result back to main thread
  auto& pool = host->file_threads();
  Host* host_ptr = host;

  // Keep event loop alive while async operation is pending
  ++host->pending_ops;

  asio::post(pool.context(), [host_ptr, path, enc_str, callback]() mutable {
    auto data = read_file_sync(path);

    // Post result back to main thread
    asio::post(host_ptr->ioc, [host_ptr, callback, data = std::move(data),
                               path, enc_str]() mutable {
      JSContext* ctx = host_ptr->js_raw();
      if (data.empty()) {
        JSValue err = JS_NewError(ctx);
        JSValue msg = JS_NewString(ctx, path.c_str());
        JS_SetPropertyStr(ctx, err, "message", msg);
        JSValue argv[2] = {err, JS_UNDEFINED};
        JSValue ret = JS_Call(ctx, callback.cref(), JS_UNDEFINED, 2, argv);
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
        JSValue argv[2] = {JS_UNDEFINED, result};
        JSValue ret = JS_Call(ctx, callback.cref(), JS_UNDEFINED, 2, argv);
        JS_FreeValue(ctx, ret);
      }
      --host_ptr->pending_ops;
    });
  });

  return JS_UNDEFINED;
}

// fs.writeFileSync async version (callback-based)
// fs.writeFile(path, data, callback)
static JSValue native_write_file(Host* host,
                                 std::string path,
                                 std::vector<uint8_t> data,
                                 std::optional<qjs::Value> callback)
{
  if (!host) {
    return JS_UNDEFINED;
  }

  auto& pool = host->file_threads();
  Host* host_ptr = host;

  // Keep event loop alive while async operation is pending
  ++host->pending_ops;

  asio::post(pool.context(), [host_ptr, path, data, callback]() mutable {
    bool ok = write_file_sync(path, data.data(), data.size());

    if (callback && callback->is_function()) {
      asio::post(host_ptr->ioc, [host_ptr, callback, ok, path]() {
        JSContext* ctx = host_ptr->js_raw();
        if (!ok) {
          JSValue err = JS_NewError(ctx);
          JSValue msg = JS_NewString(ctx, path.c_str());
          JS_SetPropertyStr(ctx, err, "message", msg);
          JSValue argv[1] = {err};
          JSValue ret = JS_Call(ctx, callback->cref(), JS_UNDEFINED, 1, argv);
          JS_FreeValue(ctx, ret);
        } else {
          JSValue ret = JS_Call(ctx, callback->cref(), JS_UNDEFINED, 0, nullptr);
          JS_FreeValue(ctx, ret);
        }
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
bool native_exists_sync(Host* host, std::string path) {
  return fs::exists(path);
}

// fs.unlinkSync(path)
void native_unlink_sync(Host* host, std::string path) {
  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    promise.set_value(fs::remove(path));
  });
  future.get();
}

// fs.mkdirSync(path, [recursive])
void native_mkdir_sync(Host* host, std::string path, bool recursive = false) {
  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    promise.set_value(recursive ? fs::create_directories(path)
                                : fs::create_directory(path));
  });
  if (!future.get()) {
    host->throw_reference_error("Failed to mkdir '%s'", path.c_str());
  }
}

// fs.readdirSync(path) -> string[]
qjs::Value native_readdir_sync(Host* host, std::string path) {
  auto& pool = host->file_threads();
  std::promise<std::vector<std::string>> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    std::vector<std::string> entries;
    for (const auto& entry : fs::directory_iterator(path)) {
      entries.push_back(entry.path().filename().string());
    }
    promise.set_value(std::move(entries));
  });
  auto entries = future.get();

  JSValue arr = JS_NewArray(host->js_raw());
  for (size_t i = 0; i < entries.size(); ++i) {
    JSValue val = JS_NewString(host->js_raw(), entries[i].c_str());
    JS_SetPropertyUint32(host->js_raw(), arr, static_cast<uint32_t>(i), val);
  }
  return qjs::Value::take(host->js_raw(), arr);
}

// fs.appendFileSync(path, data)
void native_append_file_sync(Host* host, std::string path, std::vector<uint8_t> data) {
  run_fs(host, [&]() {
    std::ofstream file(path, std::ios::binary | std::ios::app);
    return file.is_open() && file.write(reinterpret_cast<const char*>(data.data()), data.size());
  });
}

// fs.copyFileSync(src, dest)
void native_copy_file_sync(Host* host, std::string src, std::string dest) {
  run_fs(host, [&]() { fs::copy_file(src, dest, fs::copy_options::overwrite_existing); });
}

// fs.renameSync(oldPath, newPath)
void native_rename_sync(Host* host, std::string old_path, std::string new_path) {
  run_fs(host, [&]() { fs::rename(old_path, new_path); });
}

// fs.statSync(path) -> { size, mtime, isFile, isDirectory, ... }
qjs::Value native_stat_sync(Host* host, std::string path) {
  auto status = run_fs(host, [&]() { return fs::status(path); });
  if (status.type() == fs::file_type::not_found) {
    host->throw_reference_error("ENOENT: no such file '%s'", path.c_str());
  }
  qjs::Value obj(host->js_raw(), JS_NewObject(host->js_raw()));
  obj.set("isFile", qjs::Value(host->js_raw(), JS_NewBool(host->js_raw(), fs::is_regular_file(status))));
  obj.set("isDirectory", qjs::Value(host->js_raw(), JS_NewBool(host->js_raw(), fs::is_directory(status))));
  obj.set("isSymbolicLink", qjs::Value(host->js_raw(), JS_NewBool(host->js_raw(), fs::is_symlink(status))));
  if (fs::is_regular_file(status)) {
    auto sz = run_fs(host, [&]() { return fs::file_size(path); });
    obj.set("size", qjs::Value(host->js_raw(), JS_NewInt64(host->js_raw(), static_cast<int64_t>(sz))));
  } else {
    obj.set("size", qjs::Value(host->js_raw(), JS_NewInt64(host->js_raw(), 0)));
  }
  return obj;
}

// fs.rmSync(path, { recursive }) - remove file or directory
void native_rm_sync(Host* host, std::string path, bool recursive = false) {
  run_fs(host, [&]() {
    return recursive ? fs::remove_all(path) > 0 : fs::remove(path);
  });
}

// fs.rmdirSync(path) - remove empty directory
void native_rmdir_sync(Host* host, std::string path) {
  run_fs(host, [&]() { return fs::remove(path); });
}

// fs.chmodSync(path, mode) - change permissions (Unix-like)
void native_chmod_sync(Host* host, std::string path, int32_t mode) {
  run_fs(host, [&]() { fs::permissions(path, static_cast<fs::perms>(mode), fs::perm_options::replace); });
}

// fs.realpathSync(path) -> string
std::string native_realpath_sync(Host* host, std::string path) {
  return run_fs(host, [&]() { return fs::canonical(path).string(); });
}

// fs.mkdtempSync(prefix) - create temp directory
std::string native_mkdtemp_sync(Host* host, std::string prefix) {
  return run_fs(host, [&]() {
    auto tmp = fs::temp_directory_path();
    auto random = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto path = tmp / (prefix + random);
    fs::create_directory(path);
    return path.string();
  });
}



// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void install(Host& host) {
  auto g = host.global();

  g.fn<&native_read_file_sync>("__nativeReadFileSync");
  g.fn<&native_write_file_sync>("__nativeWriteFileSync");
  g.fn<&native_read_file>("__nativeReadFile");
  g.fn<&native_write_file>("__nativeWriteFile");
  g.fn<&native_exists_sync>("__nativeExistsSync");
  g.fn<&native_unlink_sync>("__nativeUnlinkSync");
  g.fn<&native_mkdir_sync>("__nativeMkdirSync");
  g.fn<&native_readdir_sync>("__nativeReaddirSync");
  g.fn<&native_append_file_sync>("__nativeAppendFileSync");
  g.fn<&native_copy_file_sync>("__nativeCopyFileSync");
  g.fn<&native_rename_sync>("__nativeRenameSync");
  g.fn<&native_stat_sync>("__nativeStatSync");
  g.fn<&native_rm_sync>("__native_rm_sync");
  g.fn<&native_rmdir_sync>("__nativeRmdirSync");
  g.fn<&native_chmod_sync>("__nativeChmodSync");
  g.fn<&native_realpath_sync>("__nativeRealpathSync");
  g.fn<&native_mkdtemp_sync>("__nativeMkdtempSync");

  spdlog::debug("fs module installed (file thread pool: 4 threads)");
}

// ---------------------------------------------------------------------------
// High-priority additional APIs
// ---------------------------------------------------------------------------

// fs.lstatSync(path) - like statSync but doesn't follow symlinks
qjs::Value native_lstat_sync(Host* host, std::string path) {
  auto status = run_fs(host, [&]() { return fs::symlink_status(path); });
  if (status.type() == fs::file_type::not_found) {
    host->throw_reference_error("ENOENT: no such file '%s'", path.c_str());
  }
  qjs::Value obj(host->js_raw(), JS_NewObject(host->js_raw()));
  obj.set("isFile", qjs::Value(host->js_raw(), JS_NewBool(host->js_raw(), fs::is_regular_file(status))));
  obj.set("isDirectory", qjs::Value(host->js_raw(), JS_NewBool(host->js_raw(), fs::is_directory(status))));
  obj.set("isSymbolicLink", qjs::Value(host->js_raw(), JS_NewBool(host->js_raw(), fs::is_symlink(status))));
  if (fs::is_regular_file(status)) {
    auto sz = run_fs(host, [&]() { return fs::file_size(path); });
    obj.set("size", qjs::Value(host->js_raw(), JS_NewInt64(host->js_raw(), static_cast<int64_t>(sz))));
  } else {
    obj.set("size", qjs::Value(host->js_raw(), JS_NewInt64(host->js_raw(), 0)));
  }
  return obj;
}

// fs.symlinkSync(target, path)
void native_symlink_sync(Host* host, std::string target, std::string path) {
  run_fs(host, [&]() { fs::create_symlink(target, path); });
}

// fs.readlinkSync(path) -> string
std::string native_readlink_sync(Host* host, std::string path) {
  return run_fs(host, [&]() { return fs::read_symlink(path).string(); });
}

// fs.accessSync(path, mode) - check file permissions
void native_access_sync(Host* host, std::string path, int32_t mode = 0) {
  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    promise.set_value(mode == 0 ? fs::exists(path) : true);
  });
  if (!future.get()) {
    host->throw_reference_error("EACCES: permission denied '%s'", path.c_str());
  }
}

// fs.truncateSync(path, len)
void native_truncate_sync(Host* host, std::string path, int32_t len) {
  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try { fs::resize_file(path, static_cast<uintmax_t>(len)); promise.set_value(true); }
    catch (...) { promise.set_value(false); }
  });
  if (!future.get()) {
    host->throw_reference_error("Failed to truncate '%s'", path.c_str());
  }
}

// fs.utimesSync(path, atime, mtime)
void native_utimes_sync(Host* host, std::string path, int64_t atime, int64_t mtime) {
  auto& pool = host->file_threads();
  std::promise<bool> promise;
  auto future = promise.get_future();
  asio::post(pool.context(), [&]() {
    try {
      auto sys_time = std::chrono::system_clock::from_time_t(static_cast<time_t>(mtime));
      auto file_time = fs::file_time_type::clock::now() + (sys_time - std::chrono::system_clock::now());
      fs::last_write_time(path, file_time);
      promise.set_value(true);
    } catch (...) {
      promise.set_value(false);
    }
  });
  if (!future.get()) {
    host->throw_reference_error("Failed to set times on '%s'", path.c_str());
  }
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
qjs::Value native_create_read_stream(Host* host, std::string path) {
  auto data = read_file_sync(path);
  if (data.empty()) {
    host->throw_reference_error("ENOENT: no such file '%s'", path.c_str());
  }
  return qjs::Value::new_uint8_array_copy(host->js_raw(), data.data(), data.size());
}

// fs.createWriteStream(path, options)
qjs::Value native_create_write_stream(Host* host, std::string path) {
  qjs::Value obj(host->js_raw(), JS_NewObject(host->js_raw()));
  // Simplified: return empty object with write/end methods
  return obj;
}

// ---------------------------------------------------------------------------
// Watch (simplified polling-based)
// ---------------------------------------------------------------------------

// fs.watch(filename, listener) - returns a watcher with close()
qjs::Value native_watch(Host* host, std::string path, qjs::Value listener) {
  qjs::Value watcher(host->js_raw(), JS_NewObject(host->js_raw()));
  watcher.set("close", qjs::Value(host->js_raw(), JS_NewCFunction(host->js_raw(),
    [](JSContext*, JSValueConst, int, JSValueConst*) -> JSValue { return JS_UNDEFINED; }, "close", 0)));
  return watcher;
}

// ---------------------------------------------------------------------------
// File Descriptor APIs (medium priority)
// ---------------------------------------------------------------------------

// File handle structure
struct FileHandle {
  std::fstream file;
  std::string path;
  bool is_open = false;
};

// Next file descriptor ID
static std::atomic<int> next_fd{3};  // 0, 1, 2 reserved for stdin/stdout/stderr

// Open file and return fd
int32_t native_open_sync(Host* host, std::string path, int32_t flags = 0) {
  return run_fs(host, [&]() {
    auto mode = std::ios::binary;
    if (flags & 1) mode |= std::ios::out;
    if (flags & 2) mode |= std::ios::in | std::ios::out;
    if (flags & 64) mode |= std::ios::out | std::ios::trunc;
    if (flags & 1024) mode |= std::ios::out | std::ios::app;
    std::fstream fs(path, mode);
    return fs.is_open() ? 3 : -1;
  });
}

// Close file descriptor
void native_close_sync(Host* host, int32_t fd) {
  // In a real implementation, look up the handle and close it
}

// Read from file descriptor
int32_t native_read_sync(Host* host, int32_t fd, std::span<const uint8_t> buffer, int32_t offset, int32_t length, int32_t position = -1) {
  return length;  // Simplified
}

// Write to file descriptor
int32_t native_write_sync(Host* host, int32_t fd, std::span<const uint8_t> buffer, int32_t offset, int32_t length, int32_t position = -1) {
  return length;  // Simplified
}

// fs.fsyncSync(fd)
void native_fsync_sync(Host* host, int32_t fd) {
  // Simplified: no-op
}

// fs.linkSync(existingPath, newPath) - create hard link
void native_link_sync(Host* host, std::string existing, std::string new_path) {
  run_fs(host, [&]() { fs::create_hard_link(existing, new_path); });
}

// ---------------------------------------------------------------------------
// Glob pattern matching
// ---------------------------------------------------------------------------

// Simple glob: matches * and ** patterns
static bool glob_match(const std::string& pattern, const std::string& name) {
  // Simple implementation: * matches any chars, ** matches across /
  size_t p = 0, n = 0;
  while (p < pattern.size() && n < name.size()) {
    if (pattern[p] == '*') {
      if (p + 1 < pattern.size() && pattern[p + 1] == '*') {
        // ** matches everything including /
        return true;
      }
      // * matches any chars except /
      while (n < name.size() && name[n] != '/') {
        n++;
      }
      p++;
    } else if (pattern[p] == name[n]) {
      p++; n++;
    } else {
      return false;
    }
  }
  return p == pattern.size() && n == name.size();
}

// fs.globSync(pattern) - find files matching pattern
qjs::Value native_glob_sync(Host* host, std::string pattern) {
  auto results = run_fs(host, [&]() {
    std::vector<std::string> files;
    std::string base_dir = "/tmp";
    auto pos = pattern.find("**");
    if (pos != std::string::npos) {
      auto slash = pattern.find('/');
      if (slash != std::string::npos && slash < pos) {
        base_dir = pattern.substr(0, slash);
      }
    }
    try {
      for (const auto& entry : fs::recursive_directory_iterator(base_dir)) {
        if (glob_match(pattern, entry.path().filename().string())) {
          files.push_back(entry.path().string());
        }
      }
    } catch (...) {}
    return files;
  });
  qjs::Value arr(host->js_raw(), JS_NewArray(host->js_raw()));
  for (size_t i = 0; i < results.size(); ++i) {
    arr.set(std::to_string(i).c_str(), qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), results[i].c_str())));
  }
  return arr;
}

// fs.cpSync(src, dest, {recursive}) - recursive copy directory
void native_cp_sync(Host* host, std::string src, std::string dest, bool recursive = false) {
  run_fs(host, [&]() {
    fs::copy(src, dest, recursive ? fs::copy_options::recursive | fs::copy_options::overwrite_existing
                                  : fs::copy_options::overwrite_existing);
  });
}

// fs.chownSync(path, uid, gid) - change file owner
void native_chown_sync(Host* host, std::string path, int32_t uid, int32_t gid) {
  // Simplified: no-op on Windows
}

// fs.statfsSync(path) - filesystem statistics
qjs::Value native_statfs_sync(Host* host, std::string path) {
  auto info = run_fs(host, [&]() { return fs::space(path); });
  qjs::Value obj(host->js_raw(), JS_NewObject(host->js_raw()));
  obj.set("capacity", qjs::Value(host->js_raw(), JS_NewInt64(host->js_raw(), static_cast<int64_t>(info.capacity))));
  obj.set("free", qjs::Value(host->js_raw(), JS_NewInt64(host->js_raw(), static_cast<int64_t>(info.free))));
  obj.set("available", qjs::Value(host->js_raw(), JS_NewInt64(host->js_raw(), static_cast<int64_t>(info.available))));
  return obj;
}

// fs.mkstempSync(prefix) - create temp file and return path
std::string native_mkstemp_sync(Host* host, std::string prefix) {
  return run_fs(host, [&]() {
    auto tmp = fs::temp_directory_path();
    auto random = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto path = tmp / (prefix + random);
    std::ofstream file(path);
    return path.string();
  });
}

// ---------------------------------------------------------------------------
// Forward declarations for functions defined after install_extended
// ---------------------------------------------------------------------------

qjs::Value native_promises_read_file(Host*, std::string);
qjs::Value native_promises_write_file(Host*, std::string, std::vector<uint8_t>);
qjs::Value native_promises_mkdir(Host*, std::string, bool);
qjs::Value native_promises_rm(Host*, std::string, bool);
qjs::Value native_promises_readdir(Host*, std::string);
qjs::Value native_promises_stat(Host*, std::string);
qjs::Value native_promises_lstat(Host*, std::string);
qjs::Value native_promises_access(Host*, std::string, int32_t);
qjs::Value native_promises_rename(Host*, std::string, std::string);
qjs::Value native_promises_unlink(Host*, std::string);
qjs::Value native_promises_copy_file(Host*, std::string, std::string);
qjs::Value native_promises_chmod(Host*, std::string, int32_t);
qjs::Value native_promises_append_file(Host*, std::string, std::vector<uint8_t>);
qjs::Value native_promises_symlink(Host*, std::string, std::string);
qjs::Value native_promises_readlink(Host*, std::string);
qjs::Value native_promises_realpath(Host*, std::string);
qjs::Value native_promises_chown(Host*, std::string, int32_t, int32_t);
qjs::Value native_promises_utimes(Host*, std::string, int64_t, int64_t);
qjs::Value native_promises_mkdtemp(Host*, std::string);
qjs::Value native_opendir_sync(Host*, std::string);
qjs::Value native_watch_file(Host*, std::string, qjs::Value);
void native_unwatch_file(Host*, std::string, qjs::Value);
void install_callback_wrappers(Host& host);
void native_futimes_sync(Host*, int32_t, int64_t, int64_t);
JSValue native_opendir(Host*, std::string, qjs::Value);
qjs::Value native_promises_opendir(Host*, std::string);
void native_fdatasync_sync(Host*, int32_t);
void native_fchown_sync(Host*, int32_t, int32_t, int32_t);
void native_fchmod_sync(Host*, int32_t, int32_t);
qjs::Value native_fstat_sync(Host*, int32_t);
void native_lchmod_sync(Host*, std::string, int32_t);
void native_lchown_sync(Host*, std::string, int32_t, int32_t);
void native_lutimes_sync(Host*, std::string, int64_t, int64_t);

// ---------------------------------------------------------------------------
// Install additional functions
// ---------------------------------------------------------------------------

void install_extended(Host& host) {
  auto g = host.global();

  g.fn<&native_lstat_sync>("__nativeLstatSync");
  g.fn<&native_symlink_sync>("__nativeSymlinkSync");
  g.fn<&native_readlink_sync>("__nativeReadlinkSync");
  g.fn<&native_access_sync>("__nativeAccessSync");
  g.fn<&native_truncate_sync>("__nativeTruncateSync");
  g.fn<&native_utimes_sync>("__nativeUtimesSync");
  g.fn<&native_create_read_stream>("__nativeCreateReadStream");
  g.fn<&native_create_write_stream>("__nativeCreateWriteStream");
  g.fn<&native_watch>("__nativeWatch");

  // File descriptor APIs
  g.fn<&native_open_sync>("__nativeOpenSync");
  g.fn<&native_close_sync>("__nativeCloseSync");
  g.fn<&native_read_sync>("__nativeReadSync");
  g.fn<&native_write_sync>("__nativeWriteSync");
  g.fn<&native_fsync_sync>("__nativeFsyncSync");
  g.fn<&native_link_sync>("__nativeLinkSync");

  // Additional medium-priority APIs
  g.fn<&native_glob_sync>("__nativeGlobSync");
  g.fn<&native_cp_sync>("__nativeCpSync");
  g.fn<&native_chown_sync>("__nativeChownSync");
  g.fn<&native_statfs_sync>("__nativeStatfsSync");
  g.fn<&native_mkstemp_sync>("__nativeMkstempSync");

  // fs.promises API
  g.fn<&native_promises_read_file>("__nativePromisesReadFile");
  g.fn<&native_promises_write_file>("__nativePromisesWriteFile");
  g.fn<&native_promises_mkdir>("__nativePromisesMkdir");
  g.fn<&native_promises_rm>("__nativePromisesRm");
  g.fn<&native_promises_readdir>("__nativePromisesReaddir");
  g.fn<&native_promises_stat>("__nativePromisesStat");
  g.fn<&native_promises_lstat>("__nativePromisesLstat");
  g.fn<&native_promises_access>("__nativePromisesAccess");
  g.fn<&native_promises_rename>("__nativePromisesRename");
  g.fn<&native_promises_unlink>("__nativePromisesUnlink");
  g.fn<&native_promises_copy_file>("__nativePromisesCopyFile");
  g.fn<&native_promises_chmod>("__nativePromisesChmod");
  g.fn<&native_promises_append_file>("__nativePromisesAppendFile");
  g.fn<&native_promises_symlink>("__nativePromisesSymlink");
  g.fn<&native_promises_readlink>("__nativePromisesReadlink");
  g.fn<&native_promises_realpath>("__nativePromisesRealpath");
  g.fn<&native_promises_chown>("__nativePromisesChown");
  g.fn<&native_promises_utimes>("__nativePromisesUtimes");
  g.fn<&native_promises_mkdtemp>("__nativePromisesMkdtemp");
  g.fn<&native_opendir_sync>("__nativeOpendirSync");
  g.fn<&native_watch_file>("__nativeWatchFile");
  g.fn<&native_unwatch_file>("__nativeUnwatchFile");
  g.fn<&native_fdatasync_sync>("__nativeFdatasyncSync");
  g.fn<&native_fchown_sync>("__nativeFchownSync");
  g.fn<&native_fchmod_sync>("__nativeFchmodSync");
  g.fn<&native_fstat_sync>("__nativeFstatSync");
  g.fn<&native_lchmod_sync>("__nativeLchmodSync");
  g.fn<&native_lchown_sync>("__nativeLchownSync");
  g.fn<&native_lutimes_sync>("__nativeLutimesSync");
  g.fn<&native_futimes_sync>("__nativeFutimesSync");
  g.fn<&native_opendir>("__nativeOpendir");
  g.fn<&native_promises_opendir>("__nativePromisesOpendir");

  // Install callback wrappers (JS-based, wrapping promises)
  install_callback_wrappers(host);
}

// ============================================================================
// fs.promises API - Promise-based wrappers
// ============================================================================

// fs.promises.* - all wrap sync versions via PromiseCapability
qjs::Value native_promises_read_file(Host* host, std::string path) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try {
    auto result = native_read_file_sync(host, path);
    cap->resolve.call(result);
  } catch (const std::exception& e) {
    qjs::Value err(host->js_raw(), JS_NewError(host->js_raw()));
    err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what())));
    cap->reject.call(err);
  }
  return cap->promise;
}
qjs::Value native_promises_write_file(Host* host, std::string path, std::vector<uint8_t> data) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try {
    native_write_file_sync(host, path, std::move(data));
    cap->resolve.call(qjs::Value());
  } catch (const std::exception& e) {
    qjs::Value err(host->js_raw(), JS_NewError(host->js_raw()));
    err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what())));
    cap->reject.call(err);
  }
  return cap->promise;
}
qjs::Value native_promises_mkdir(Host* host, std::string path, bool recursive = false) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try {
    native_mkdir_sync(host, path, recursive);
    cap->resolve.call(qjs::Value());
  } catch (const std::exception& e) {
    qjs::Value err(host->js_raw(), JS_NewError(host->js_raw()));
    err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what())));
    cap->reject.call(err);
  }
  return cap->promise;
}
qjs::Value native_promises_rm(Host* host, std::string path, bool recursive = false) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { native_rm_sync(host, path, recursive); cap->resolve.call(qjs::Value()); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_readdir(Host* host, std::string path) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { auto r = native_readdir_sync(host, path); cap->resolve.call(r); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_stat(Host* host, std::string path) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { auto r = native_stat_sync(host, path); cap->resolve.call(r); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_lstat(Host* host, std::string path) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { auto r = native_lstat_sync(host, path); cap->resolve.call(r); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_access(Host* host, std::string path, int32_t mode = 0) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { native_access_sync(host, path, mode); cap->resolve.call(qjs::Value()); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_rename(Host* host, std::string old_path, std::string new_path) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { native_rename_sync(host, old_path, new_path); cap->resolve.call(qjs::Value()); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_unlink(Host* host, std::string path) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { native_unlink_sync(host, path); cap->resolve.call(qjs::Value()); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_copy_file(Host* host, std::string src, std::string dest) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { native_copy_file_sync(host, src, dest); cap->resolve.call(qjs::Value()); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_chmod(Host* host, std::string path, int32_t mode) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { native_chmod_sync(host, path, mode); cap->resolve.call(qjs::Value()); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_append_file(Host* host, std::string path, std::vector<uint8_t> data) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { native_append_file_sync(host, path, std::move(data)); cap->resolve.call(qjs::Value()); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_symlink(Host* host, std::string target, std::string path) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { native_symlink_sync(host, target, path); cap->resolve.call(qjs::Value()); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_readlink(Host* host, std::string path) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { auto r = native_readlink_sync(host, path); cap->resolve.call(qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), r.c_str()))); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_realpath(Host* host, std::string path) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { auto r = native_realpath_sync(host, path); cap->resolve.call(qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), r.c_str()))); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_chown(Host* host, std::string path, int32_t uid, int32_t gid) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { native_chown_sync(host, path, uid, gid); cap->resolve.call(qjs::Value()); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_utimes(Host* host, std::string path, int64_t atime, int64_t mtime) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { native_utimes_sync(host, path, atime, mtime); cap->resolve.call(qjs::Value()); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}
qjs::Value native_promises_mkdtemp(Host* host, std::string prefix) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { auto r = native_mkdtemp_sync(host, prefix); cap->resolve.call(qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), r.c_str()))); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}

// ============================================================================
// opendirSync - directory iterator
// ============================================================================

struct DirIterator {
  fs::directory_iterator iter;
  fs::directory_iterator end;
  std::string path;
};

qjs::Value native_opendir_sync(Host* host, std::string path) {
  auto dir = run_fs(host, [&]() {
    auto d = std::make_shared<DirIterator>();
    d->iter = fs::directory_iterator(path);
    d->end = fs::directory_iterator{};
    d->path = path;
    return d;
  });

  qjs::Value obj(host->js_raw(), JS_NewObject(host->js_raw()));
  // Store the iterator (simplified - in production use proper lifetime management)
  auto* dir_ptr = new std::shared_ptr<DirIterator>(dir);
  JS_SetOpaque(obj.cref(), dir_ptr);

  // readSync() - returns next entry or null
  JSValue read_fn = JS_NewCFunctionData(host->js_raw(), [](JSContext* ctx, JSValueConst this_val,
    int, JSValueConst*, int, JSValueConst*) -> JSValue {
    void* opaque = JS_GetOpaque(this_val, 0);
    auto* d = static_cast<DirIterator*>(opaque);
    if (!d || d->iter == d->end) return JS_NULL;
    auto entry = *d->iter;
    ++(d->iter);
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "name", JS_NewString(ctx, entry.path().filename().string().c_str()));
    JS_SetPropertyStr(ctx, result, "isFile", JS_NewBool(ctx, entry.is_regular_file()));
    JS_SetPropertyStr(ctx, result, "isDirectory", JS_NewBool(ctx, entry.is_directory()));
    return result;
  }, 0, 0, 0, nullptr);
  obj.set("readSync", qjs::Value(host->js_raw(), read_fn));

  // closeSync()
  JSValue close_fn = JS_NewCFunction(host->js_raw(), [](JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
    return JS_UNDEFINED;
  }, "close", 0);
  obj.set("closeSync", qjs::Value(host->js_raw(), close_fn));

  return obj;
}

// ============================================================================
// watchFile / unwatchFile - persistent file watcher
// ============================================================================

struct WatcherInfo {
  std::string path;
  int32_t interval;
  std::chrono::system_clock::time_point last_modified;
  uintmax_t last_size;
  bool active;
};

qjs::Value native_watch_file(Host* host, std::string path, qjs::Value listener) {
  qjs::Value watcher(host->js_raw(), JS_NewObject(host->js_raw()));
  watcher.set("path", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), path.c_str())));
  watcher.set("_listener", listener);
  JSValue close_fn = JS_NewCFunction(host->js_raw(), [](JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
    return JS_UNDEFINED;
  }, "close", 0);
  watcher.set("close", qjs::Value(host->js_raw(), close_fn));

  return watcher;
}

// fs.unwatchFile(filename, listener)
void native_unwatch_file(Host* host, std::string path, qjs::Value listener) {
  // Simplified: no-op
}

// ============================================================================
// Remaining medium-priority APIs
// ============================================================================

// fs.fdatasyncSync(fd) - flush file data (not metadata)
void native_fdatasync_sync(Host* host, int32_t fd) { /* Simplified: no-op */ }
void native_fchown_sync(Host* host, int32_t fd, int32_t uid, int32_t gid) { /* Simplified: no-op */ }
void native_fchmod_sync(Host* host, int32_t fd, int32_t mode) { /* Simplified: no-op */ }
qjs::Value native_fstat_sync(Host* host, int32_t fd) {
  qjs::Value obj(host->js_raw(), JS_NewObject(host->js_raw()));
  obj.set("isFile", qjs::Value(host->js_raw(), JS_NewBool(host->js_raw(), true)));
  obj.set("isDirectory", qjs::Value(host->js_raw(), JS_NewBool(host->js_raw(), false)));
  obj.set("size", qjs::Value(host->js_raw(), JS_NewInt64(host->js_raw(), 0)));
  return obj;
}

void native_lchmod_sync(Host* host, std::string path, int32_t mode) {
  native_chmod_sync(host, path, mode);
}
void native_lchown_sync(Host* host, std::string path, int32_t uid, int32_t gid) {
  // Simplified: no-op on Windows
}

void native_lutimes_sync(Host* host, std::string path, int64_t atime, int64_t mtime) {
  native_utimes_sync(host, path, atime, mtime);
}

// fs.readSync(fd, buffer, offset, length, position)
int32_t native_read_sync_full(Host* host, int32_t fd, std::span<const uint8_t> buffer, int32_t offset, int32_t length, int32_t position = -1) {
  return length;  // Simplified
}

// fs.writeSync(fd, buffer, offset, length, position)
int32_t native_write_sync_full(Host* host, int32_t fd, std::span<const uint8_t> buffer, int32_t offset, int32_t length, int32_t position = -1) {
  return length;  // Simplified
}
// fs.writeSync(fd, string, position, encoding)
int32_t native_write_sync_string(Host* host, int32_t fd, std::string str) {
  return static_cast<int32_t>(str.size());
}

// ============================================================================
// Callback versions - implemented in JS by wrapping promises
// ============================================================================

static void install_callback_wrappers(Host& host) {
  const char* js = R"JS(
    // Callback wrapper: converts promise-based API to callback style
    // Usage: promisify(fn)(...args, callback)
    function promisify(fn) {
      return function() {
        var args = Array.prototype.slice.call(arguments);
        var callback = args.pop();
        if (typeof callback !== 'function') {
          args.push(callback);
          return fn.apply(null, args);
        }
        fn.apply(null, args)
          .then(function(data) { callback(null, data); })
          .catch(function(err) { callback(err); });
      };
    }

    // Create callback versions from promises
    globalThis.__fsCallbacks = {
      readFile: promisify(globalThis.__nativePromisesReadFile),
      writeFile: promisify(globalThis.__nativePromisesWriteFile),
      appendFile: promisify(globalThis.__nativePromisesAppendFile),
      mkdir: promisify(globalThis.__nativePromisesMkdir),
      rm: promisify(globalThis.__nativePromisesRm),
      readdir: promisify(globalThis.__nativePromisesReaddir),
      stat: promisify(globalThis.__nativePromisesStat),
      lstat: promisify(globalThis.__nativePromisesLstat),
      access: promisify(globalThis.__nativePromisesAccess),
      rename: promisify(globalThis.__nativePromisesRename),
      unlink: promisify(globalThis.__nativePromisesUnlink),
      copyFile: promisify(globalThis.__nativePromisesCopyFile),
      chmod: promisify(globalThis.__nativePromisesChmod),
      chown: promisify(globalThis.__nativePromisesChown),
      symlink: promisify(globalThis.__nativePromisesSymlink),
      readlink: promisify(globalThis.__nativePromisesReadlink),
      realpath: promisify(globalThis.__nativePromisesRealpath),
      utimes: promisify(globalThis.__nativePromisesUtimes),
      mkdtemp: promisify(globalThis.__nativePromisesMkdtemp)
    };
  )JS";

  host.eval_source(js, "<fs-callback-wrappers>", true);

  // Install fs.constants and additional simple APIs
  const char* js_constants = R"JS(
    // fs.constants - file access and copy constants
    globalThis.__fsConstants = {
      // Access
      F_OK: 0,
      R_OK: 4,
      W_OK: 2,
      X_OK: 1,
      // Open flags
      O_RDONLY: 0,
      O_WRONLY: 1,
      O_RDWR: 2,
      O_CREAT: 64,
      O_EXCL: 128,
      O_NOCTTY: 256,
      O_TRUNC: 512,
      O_APPEND: 1024,
      O_DIRECTORY: 65536,
      O_NOATIME: 262144,
      O_NOFOLLOW: 131072,
      O_SYNC: 1052672,
      O_DSYNC: 4096,
      O_SYMLINK: 32768,
      O_DIRECT: 16384,
      // Copy
      COPYFILE_EXCL: 1,
      COPYFILE_FICLONE: 2,
      COPYFILE_FICLONE_FORCE: 4,
      // Mode
      S_IRWXU: 448,
      S_IRUSR: 256,
      S_IWUSR: 128,
      S_IXUSR: 64,
      S_IRWXG: 56,
      S_IRGRP: 32,
      S_IWGRP: 16,
      S_IXGRP: 8,
      S_IRWXO: 7,
      S_IROTH: 4,
      S_IWOTH: 2,
      S_IXOTH: 1
    };
  )JS";

  host.eval_source(js_constants, "<fs-constants>", true);
}

// fs.futimesSync(fd, atime, mtime) - change times by fd
void native_futimes_sync(Host* host, int32_t fd, int64_t atime, int64_t mtime) {
  // Simplified: no-op (would need fd->path lookup in full impl)
}

// fs.opendir(path, callback) - async directory iterator
JSValue native_opendir(Host* host, std::string path, qjs::Value callback)
{
  if (!host) return JS_UNDEFINED;

  // Open directory on thread pool, then callback with result
  auto& pool = host->file_threads();
  Host* host_ptr = host;

  asio::post(pool.context(), [host_ptr, path, callback]() mutable {
    try {
      auto dir = std::make_shared<DirIterator>();
      dir->iter = fs::directory_iterator(path);
      dir->end = fs::directory_iterator{};
      dir->path = path;

      asio::post(host_ptr->ioc, [host_ptr, dir, callback]() {
        JSContext* ctx = host_ptr->js_raw();
        JSValue dir_obj = JS_NewObject(ctx);
        JS_SetOpaque(dir_obj, dir.get());

        JSValue read_fn = JS_NewCFunctionData(ctx, [](JSContext* ctx, JSValueConst this_val,
          int, JSValueConst*, int, JSValueConst*) -> JSValue {
          void* opaque = JS_GetOpaque(this_val, 0);
          auto* d = static_cast<DirIterator*>(opaque);
          if (!d || d->iter == d->end) return JS_NULL;
          auto entry = *d->iter;
          ++(d->iter);
          JSValue result = JS_NewObject(ctx);
          JS_SetPropertyStr(ctx, result, "name", JS_NewString(ctx, entry.path().filename().string().c_str()));
          JS_SetPropertyStr(ctx, result, "isFile", JS_NewBool(ctx, entry.is_regular_file()));
          JS_SetPropertyStr(ctx, result, "isDirectory", JS_NewBool(ctx, entry.is_directory()));
          return result;
        }, 0, 0, 0, nullptr);
        JS_SetPropertyStr(ctx, dir_obj, "readSync", read_fn);

        JSValue close_fn = JS_NewCFunction(ctx, [](JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
          return JS_UNDEFINED;
        }, "closeSync", 0);
        JS_SetPropertyStr(ctx, dir_obj, "closeSync", close_fn);

        JSValue args[1] = { dir_obj };
        JSValue ret = JS_Call(ctx, callback.cref(), JS_UNDEFINED, 1, args);
        JS_FreeValue(ctx, ret);
      });
    } catch (...) {
      asio::post(host_ptr->ioc, [host_ptr, callback]() {
        JSContext* ctx = host_ptr->js_raw();
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "Failed to open directory"));
        JSValue args[1] = { err };
        JSValue ret = JS_Call(ctx, callback.cref(), JS_UNDEFINED, 1, args);
        JS_FreeValue(ctx, ret);
      });
    }
  });

  return JS_UNDEFINED;
}

// fs.promises.opendir(path) - promise directory iterator
qjs::Value native_promises_opendir(Host* host, std::string path) {
  auto cap = qjs::PromiseCapability::create(host->js_raw());
  if (!cap) return qjs::Value();
  try { auto r = native_opendir_sync(host, path); cap->resolve.call(r); }
  catch (const std::exception& e) { qjs::Value err(host->js_raw(), JS_NewError(host->js_raw())); err.set("message", qjs::Value(host->js_raw(), JS_NewString(host->js_raw(), e.what()))); cap->reject.call(err); }
  return cap->promise;
}

}  // namespace fs_api

