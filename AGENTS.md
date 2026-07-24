# AGENTS.md — asio-quickjs-ng

> This file is written for AI coding agents. It summarizes the project architecture, build system, testing strategy, and conventions so you can be productive without prior knowledge of the repository.

## Project overview

`asio-quickjs-ng` is a small C++23 runtime that embeds QuickJS-NG and drives async I/O with asio. It is **not** a Node.js clone; the goal is to expose a minimal, stable JavaScript API surface (timers, `fetch`, and eventually inbound TCP/HTTP) while keeping business logic in JS and concurrency in the C++ event loop.

- C++ provides: async I/O runtime, thin JS bindings, and high concurrency from a single-threaded event loop.
- JS provides: business logic, routing, and protocol orchestration.
- Non-goal: full npm/Express compatibility.

Current implemented features:

- `setTimeout`, `clearTimeout`, `setInterval`, `clearInterval`, `print`, `console.*` (in `src/host.cpp`).
- `fetch` / `Request` / `Response` / `Headers` / `AbortController` (polyfills in `src/js/*.js`; C++ transport in `src/fetch.cpp`, `src/curl_http.hpp`, and `src/curl_runtime.cpp`).
- `base64` / `base64.encode` / `base64.decode` (in `src/host.cpp`).
- `binaryStore.put` / `binaryStore.take`: global process-wide binary blob store (in `src/binary_store.*`). `put(bytes)` returns a UUID id; `take(id)` consumes and returns a `Uint8Array`, or `null` if missing. Entries unused for 15 minutes are dropped by lazy GC on put/take.
- `Buffer` polyfill (in `src/js/buffer.js`, bundled and minified from the npm `buffer` package).
- `crypto` module: hash/hmac (`md5`, `sha1`, `sha256`, `sha512`, `createHash`, `createHmac`), AES-ECB/CBC/GCM, `randomBytes`, `randomUUID`, `timingSafeEqual`, `pbkdf2`/`pbkdf2Sync` (C++ in `src/crypto.cpp`; JS wrapper in `src/js/crypto.js`).
- Per-request `curl_http::Client` created inside `fetch_api::async_fetch` and discarded after the request completes.
- `data:` and `about:` scheme handling in JS.
- C++20 coroutine channels (`mpsc`, `oneshot`) in `src/channel.hpp` (thread-safe; `tx.send` non-blocking, `co_await rx.recv()`).
- WPT-style fetch test runner for official Web Platform Tests.
- Dynamic function registry: register C++ functions from C++ and call them from JS via `call(name, ...args)`. Supports both sync (`Host::register_function`) and async (`Host::register_async_function`) handlers. Functions can also be registered globally (`Host::register_global_function`, `FunctionRegistry::register_global_function`) and are shared across all Host instances; the global registry is protected by a `std::shared_mutex` read-write lock so multiple concurrent Host instances can safely use them concurrently. Registered callbacks can optionally receive a `Host*` parameter to identify the caller, which is useful for per-instance data isolation (`src/function_registry.hpp/.cpp`).
- A unique ID per `Host` instance, defaulting to UUID v4; optionally supplied via `Host(std::string id)`. Exposed to JS as `globalThis.__hostID`.
- Host job scheduler (`HostManager` in `src/host_manager.*`, wire types in `src/job.hpp`): create Hosts, `submit(host_id, fn, args)` from any thread, oneshot result, soft cancel. Design: `docs/host-job-scheduler.md`.
- **Real Hono v4 framework** (`src/js/hono.bundle.js`): bundled from npm `hono@4.12.30` via esbuild. Exposed to JS via the module loader as `import { Hono from 'hono'`. Supports routing, nested routes, route params, middleware, error handling, and all standard Hono APIs.
- **ES module loader** (`src/qjs_module.cpp`): QuickJS-NG `JS_SetModuleLoaderFunc` implementation that resolves in-memory modules. Register built-in modules via `qjs::Runtime::register_module(name, source)`.
- **HTTP/1.1 server** (`src/net/http_server.hpp/.cpp`): per-connection `HttpSession` with llhttp-based request parsing, keep-alive support, and dynamic response dispatch. Integrated with Hono via `Host::start_http_server(port, handler)`.
- **Streaming / chunked transfer encoding**: `globalThis.stream(callback, options)` initiates a chunked response. The callback receives `write(data)` and `end()` functions. Uses synchronous `asio::write` (safe for single-threaded event loop).
- **JS helpers** (`src/js/http.js`): `globalThis.createServer(handler)` and `globalThis.__httpHandler` bridge for HTTP server creation.

## Technology stack

| Layer | Libraries | Purpose |
|-------|-----------|---------|
| C++ standard | C++23 | Core language. |
| Build system | CMake >= 3.23 + local vcpkg | `vcpkg.json` declares deps; `setup-vcpkg.ps1` clones vcpkg at a pinned tag. |
| JS engine | quickjs-ng (via vcpkg) | Embedded ES2020+ runtime. |
| Async I/O | asio (via vcpkg) | Event loop, sockets, timers. |
| HTTP client | libcurl (via vcpkg) | Outbound HTTP/HTTPS through `curl_multi_*`. |
| TLS / crypto | OpenSSL (via vcpkg) | Static TLS/crypto for libcurl. |
| Coroutines | async_simple (via vcpkg) | `Lazy<void>` coroutines scheduled on asio. |
| Logging | spdlog (via vcpkg) | Runtime logs. |
| JSON | nlohmann/json (via vcpkg) | Dynamic JSON DOM. |
| Reflection | reflect-cpp (via vcpkg) | Struct ↔ JSON via reflection (bundles yyjson). |
| Testing | Google Test (via vcpkg) | C++ unit tests. |
| ID generation | boost-uuid (via vcpkg) | Header-only `boost::uuids` used for the default per-instance Host ID. |
| JS fixtures | Node.js | WPT network test server (`tests/wpt/node_test_server.mjs`). |

## Directory layout

```text
asio-quickjs-ng/
├── CMakeLists.txt          # Build definition (find_package + vcpkg)
├── vcpkg.json              # vcpkg manifest: dependency list
├── setup-vcpkg.ps1         # Clone vcpkg at a pinned tag and install deps
├── build.ps1               # Windows build helper (MSVC + Ninja)
├── build-hono.ps1          # Bundle Hono from npm (run after setup-vcpkg if updating Hono)
├── compile_commands.json   # Generated by CMake and copied to the project root by build.ps1
├── demo.js                 # Simple JS demo: timer + fetch
├── hono.js                 # Hono + streaming demo (main HTTP server example)
├── vcpkg/                  # Local vcpkg clone (ignored by git)
├── third_party/hono/       # npm hono install + esbuild bundle (ignored by git)
├── src/
│   ├── host.hpp/.cpp       # Runtime: io_context, QuickJS, pending-op counter, and main loop
│   ├── host_manager.hpp/.cpp # Multi-Host manager: submit/cancel jobs by host_id
│   ├── job.hpp             # Job/Result wire types (no JSValue across Host boundary)
│   ├── curl_runtime.hpp/.cpp # libcurl multi-handle driver: socket/timer watches (per-request lifetime)
│   ├── fetch.hpp/.cpp      # __nativeFetch, async_fetch, embedded JS bootstrap
│   ├── crypto.hpp/.cpp     # OpenSSL-backed crypto primitives exposed to JS
│   ├── binary_store.hpp/.cpp # Global binary blob store (put/take + TTL GC)
│   ├── function_registry.hpp/.cpp # Dynamic call(name, ...args) registry
│   ├── curl_http.hpp       # libcurl Global/Easy/Multi/Transfer wrappers
│   ├── asio_executor.hpp   # async_simple Executor → asio
│   ├── channel.hpp         # Thread-safe C++20 coroutine mpsc/oneshot channels
│   ├── qjs.hpp             # RAII wrappers around QuickJS C API + binding helpers
│   ├── qjs_module.cpp      # ES module loader (JS_SetModuleLoaderFunc)
│   ├── main.cpp            # CLI entry point: asio_qjs.exe
│   ├── net/
│   │   ├── http_server.hpp # HTTP/1.1 server + HttpSession (llhttp, keep-alive, streaming)
│   │   └── http_server.cpp
│   └── js/                 # Embedded JS polyfills (bundled into js_embedded.hpp by cmake/embed_js.cmake)
│       ├── abort.js
│       ├── text-encoding-polyfill.js
│       ├── whatwg-url-polyfill.js
│       ├── body_polyfill.js
│       ├── buffer.js
│       ├── crypto.js
│       ├── headers.js
│       ├── request.js
│       ├── response.js
│       ├── fetch.js
│       ├── http.js         # createServer / __httpHandler bridge
│       └── hono.bundle.js # Real Hono v4 (esbuild bundle from npm)
├── tests/
│   ├── test_channel.cpp
│   ├── test_two_qjs.cpp
│   ├── test_fetch.cpp       # Local fetch smoke tests (uses Node.js fixture server)
│   ├── test_base64.cpp      # Base64 encode/decode and zero-copy convention tests
│   ├── test_crypto.cpp      # Crypto module tests (hash, hmac, AES, random, pbkdf2)
│   ├── test_function_registry.cpp
│   ├── test_json.cpp
│   ├── test_host_id.cpp
│   ├── test_http.cpp        # HTTP server basic tests
│   ├── test_wpt_fetch.cpp
│   ├── node_fixture.hpp     # Shared helper to launch the Node.js test server
│   └── wpt/                 # WPT runner support
│       ├── manifest.txt
│       ├── shell_bootstrap.js
│       ├── testharnessreport.js
│       ├── node_test_server.mjs
│       └── local/*.any.js
├── cmake/
│   ├── embed_js.cmake      # Generate js_embedded.hpp from JS polyfills
│   └── embed_hono.cmake     # Generate hono_embedded.hpp from hono.bundle.js
└── docs/
│   ├── server-runtime.md   # Roadmap for inbound TCP/HTTP server
│   ├── wpt-fetch.md        # How to run WPT fetch tests
│   └── wpt-fetch-status.md # Current pass/fail status
```

## Build and run commands

### Prerequisites (Windows)

- Visual Studio 2022 with C++ workload (provides cl.exe, MSVC toolchain).
- Ninja on `PATH` (e.g., `winget install Ninja-build.Ninja`).
- Node.js on `PATH` (only needed for WPT fetch tests).
- Internet connection on first build to fetch dependencies.

### Configure and build

First, run the vcpkg setup script to clone vcpkg at a pinned tag and install all dependencies (this is a one-time step and may take a while because it builds from source):

```powershell
.\setup-vcpkg.ps1
```

Then build with the included PowerShell helper. `build.ps1` automatically enters the Visual Studio Developer Shell so the MSVC compiler is available:

```powershell
.\build.ps1
```

Or manually from a Developer PowerShell for VS 2022:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
  -DCMAKE_TOOLCHAIN_FILE=".\vcpkg\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DVCPKG_INSTALLED_DIR=".\vcpkg_installed"
cmake --build build -j
```

### Build targets

| Target | Output | Description |
|--------|--------|-------------|
| `asio_qjs` | `build/asio_qjs.exe` | CLI runtime. |
| `test_channel` | `build/test_channel.exe` | Channel tests. |
| `test_two_qjs` | `build/test_two_qjs.exe` | Two-VM interop test. |
| `test_fetch` | `build/test_fetch.exe` | Local fetch smoke tests. |
| `test_base64` | `build/test_base64.exe` | Base64 and zero-copy binding tests. |
| `test_binary_store` | `build/test_binary_store.exe` | Global binaryStore put/take/GC tests. |
| `test_crypto` | `build/test_crypto.exe` | Crypto module tests. |
| `test_json` | `build/test_json.exe` | JSON library sanity tests. |
| `test_host_id` | `build/test_host_id.exe` | Per-instance Host ID tests. |
| `test_host_manager` | `build/test_host_manager.exe` | HostManager submit/cancel tests. |
| `test_http` | `build/test_http.exe` | HTTP server basic tests. |
| `test_wpt_fetch` | `build/test_wpt_fetch.exe` | Official WPT fetch runner. |

### Run the demo

```powershell
.\build\asio_qjs.exe demo.js
```

You can pass any JS file as the first argument:

```powershell
.\build\asio_qjs.exe path/to/script.js
```

## Test commands

### Run all unit tests

```powershell
cd build
ctest --output-on-failure
```

Or run executables directly:

```powershell
.\build\test_channel.exe
.\build\test_two_qjs.exe
.\build\test_json.exe
.\build\test_fetch.exe
.\build\test_base64.exe
.\build\test_crypto.exe
.\build\test_host_id.exe
```

### Run WPT fetch tests

WPT tests require a sparse checkout of the official web-platform-tests repo under `third_party/wpt/`:

```powershell
# One-time checkout
git clone --filter=blob:none --sparse --depth 1 `
  https://github.com/web-platform-tests/wpt.git third_party/wpt
cd third_party/wpt
git sparse-checkout set fetch/api resources common
```

Then build and run:

```powershell
cmake --build build --target test_wpt_fetch
.\build\test_wpt_fetch.exe
# Or via ctest:
ctest --test-dir build -R WptFetch --output-on-failure
```

The runner starts `tests/wpt/node_test_server.mjs` as a network fixture, reads `tests/wpt/manifest.txt`, and executes each listed test file. The latest known result is documented in `docs/wpt-fetch-status.md`.

### Adding tests

- C++ unit tests: add `tests/test_*.cpp` and register it in `CMakeLists.txt` via `add_qjs_gtest(name tests/test_*.cpp)`.
- WPT-style tests: add `.any.js` files under `tests/wpt/local/` and list them in `tests/wpt/manifest.txt` with the `local:` prefix.
- JS smoke tests: either write a standalone JS file run by `asio_qjs.exe`, or add an async test harness in `tests/test_fetch.cpp`.

## Runtime architecture

### Concurrency model

- Single-threaded `asio::io_context` with one thread.
- C++ coroutines (`async_simple::coro::Lazy`) schedule onto the `io_context` through `AsioExecutor`.
- `Host::run_loop()` drives the loop until `pending_ops == 0` and no QuickJS jobs are pending.

### Key abstractions

- **`Host`** (`src/host.hpp`): owns `io_context`, QuickJS `Runtime`/`Context`, pending-op counter, and the main loop. Knows nothing about libcurl directly; it only keeps `next_fetch_id` and a map of in-flight `curl_http::Transfer*` pointers so that JS can abort a request by id. Provides `spawn_lazy`, `block_on`, `eval_source`, `eval_file`, `eval_module`, and `async_sleep`. The event loop now also checks `http_active` flag to keep running while HTTP server is listening.
- **`FunctionRegistry`** (`src/function_registry.hpp/.cpp`): stores C++ functions callable from JS via `call(name, ...args)`. Each `Host` has a per-instance registry, and there is a separate global registry shared by all Host instances. The global registry is protected by a `std::shared_mutex` read-write lock so multiple concurrent Host instances can safely register and call global functions. Callbacks may include a `Host*` parameter (any position, no matching JS argument) to identify the calling instance, for example to key per-Host state with `host->id()`.
- **`qjs::Value` / `qjs::Context`** (`src/qjs.hpp`): RAII wrappers and rquickjs-like binding helpers (`g.fn<&fn>("name")`, `g.obj("console", ...)`). Supports injecting `Host*` via context opaque.
- **`qjs::Runtime`** (`src/qjs.hpp`): owns QuickJS runtime, sets up module loader (`JS_SetModuleLoaderFunc`). Register in-memory modules via `static register_module(name, source)`.
- **`curl_http::Client`** (`src/curl_runtime.hpp/.cpp`): owns one `curl_multi` handle, asio socket watches, and the timeout timer. A new `Client` is constructed for each fetch request, passed the `Host`'s `AsioExecutor`, and destroyed once the request finishes.
- **`curl_http::Transfer`** (`src/curl_http.hpp`): represents one outbound HTTP request. Created per fetch call, added to its per-request `Client`, and deleted either by the Client's completion callback or by the JS abort path.
- **`fetch_api`** (`src/fetch.hpp`): `async_fetch` (C++ coroutine) and `native_fetch_fn` / `native_fetch_abort_fn` (JS-facing C functions).
- **`HttpServer`** (`src/net/http_server.hpp/.cpp`): HTTP/1.1 server with per-connection `HttpSession`. Uses llhttp for request parsing, supports keep-alive and chunked transfer encoding. Created via `Host::start_http_server(port, handler)`.
- **`HttpSession`** (`src/net/http_server.hpp/.cpp`): per-connection state. Handles request parsing, dispatches to JS handler, sends responses. Supports `send_response()` for buffered responses and `begin_chunked()` / `write_chunk()` / `finish_chunked()` for streaming.
- **Channels** (`src/channel.hpp`): thread-safe Tokio-style `co::mpsc::unbounded<T>()` and `co::oneshot::channel<T>()`. `send` is non-blocking and returns `bool`; `co_await rx.recv()` suspends the coroutine (and, when used inside `Lazy` with an executor, resumes via that executor).

### JS bootstrap order

`Host::install_runtime` loads the core embedded JS polyfills from `build/generated/js_embedded.hpp`:

1. `js/abort.js` — `AbortController`, `AbortSignal`, `DOMException`.
2. `js/text-encoding-polyfill.js` — `TextEncoder` and `TextDecoder` globals.
3. `js/whatwg-url-polyfill.js` — full WHATWG `URL` and `URLSearchParams`.
4. `js/body_polyfill.js` — minimal `Blob`, `URLSearchParams`, `FormData`, `URL`, `ReadableStream`.
5. `js/buffer.js` — `Buffer` global.
6. `js/http.js` — `createServer` / `__httpHandler` bridge for HTTP server.

Feature modules then load their own JS wrappers via `Host::install_bootstrap_js` after registering their native functions:

- `crypto_api::install` registers `__nativeCrypto` and loads `js/crypto.js`.
- `fetch_api::install` registers `__nativeFetch` / `__nativeFetchAbort` and loads `js/headers.js`, `js/request.js`, `js/response.js`, `js/fetch.js`.

`Host::register_builtin_modules()` (called from the constructor) registers the Hono framework as an in-memory module:
- `qjs::Runtime::register_module("hono", ...)` — makes `import { Hono } from 'hono'` work.

Typical initialization order is `install_runtime()` → `crypto_api::install()` → `fetch_api::install()`.

If you edit a `.js` file, you must rebuild for `cmake/embed_js.cmake` to regenerate `build/generated/js_embedded.hpp`.

If you edit `src/js/hono.bundle.js`, you must rebuild for `cmake/embed_hono.cmake` to regenerate `build/generated/hono_embedded.hpp`. Run `.\build-hono.ps1` to regenerate the bundle from npm.

## Code style guidelines

### C++

- Standard: C++23; `fetch.cpp` includes `build/generated/js_embedded.hpp` produced by `cmake/embed_js.cmake`.
- Naming: `snake_case` for functions/variables, `PascalCase` for types/classes.
- Warnings: `-Wall -Wextra -fcoroutines` on GCC/Clang; `/W3 /permissive-` on MSVC; `-Wno-maybe-uninitialized` is enabled globally for GCC/Clang.
- Keep C++ bindings thin: type conversion, lifetime management, and async dispatch. Business logic belongs in JS.
- RAII is strongly preferred; raw `JSValue` ownership is wrapped in `qjs::Value`.
- Public headers in `src/` are included with `"file.hpp"`. Third-party headers are angle-bracket included.

### JavaScript

- Polyfills are written in plain ES2020-ish JS without modules, targeting QuickJS-NG.
- Files are wrapped in `(function (global) { ... })(globalThis)` to avoid leaking locals.
- Use `var` consistently inside polyfills to avoid scoping surprises with the current engine setup.
- WPT test files use `test()` and `promise_test()` from the official `testharness.js`.

### CMake / build

- All dependencies are declared in `vcpkg.json` and installed locally via `vcpkg` (cloned by `setup-vcpkg.ps1`). Do not add submodules.
- `ASIO_QJS_BUILD_TESTS` (default `ON`) controls whether gtest tests are built.
- `CURL_STATICLIB`, `_WIN32_WINNT=0x0A00` and ASIO standalone definitions are set on `asio_qjs_core`.
- On Windows, link `ws2_32`, `mswsock`, `bcrypt`, `advapi32`, `crypt32`, `ntdll`.
- The `vcpkg/` directory is ignored by Git; it is bootstrapped by `setup-vcpkg.ps1` at the pinned tag configured in that script.
- The build uses the `x64-windows-static` vcpkg triplet and the MSVC toolchain (Visual Studio 2022) via Ninja.

## Testing instructions

1. Build with `cmake --build build -j`.
2. Run `test_channel`, `test_two_qjs`, `test_json`, `test_fetch`, `test_host_id` from the build directory.
3. For WPT tests, ensure `third_party/wpt/resources/testharness.js` exists (sparse checkout) and run `test_wpt_fetch.exe`.
4. New fetch behavior should be covered by both:
   - a local C++ smoke test in `tests/test_fetch.cpp` if it involves real network I/O, and
   - a WPT-style `.any.js` file if it matches a web specification.
5. Network tests that rely on unstable public internet should `GTEST_SKIP` or use loopback fixtures.

## Security considerations

- The runtime is intentionally minimal and does **not** implement a full browser sandbox. Do not run untrusted JS.
- `fetch` currently uses libcurl with `CURLOPT_SSL_OPTIONS` set to `CURLSSLOPT_NATIVE_CA` when available, but certificate verification details depend on the libcurl build.
- Outbound requests use the default `User-Agent: asio-quickjs-ng/0.1`.
- The C++ binding layer exposes raw pointers (`Host*` via context opaque) and native functions to JS; malformed JS should not crash the process, but the surface is small and new bindings need careful lifetime review.
- There is no filesystem API exposed to JS beyond loading the initial script.

## Common gotchas

- **`vcpkg` is local and Git-ignored**: run `setup-vcpkg.ps1` first; the `vcpkg/` directory is not committed.
- **First vcpkg install is slow**: it compiles dependencies from source (OpenSSL, curl, QuickJS-NG, etc.).
- **`#embed` caching**: editing `src/js/*.js` requires a rebuild for `cmake/embed_js.cmake` to regenerate `build/generated/js_embedded.hpp`.
- **WPT runner timing**: `eval_source(..., false)` is used when loading bootstrap/testharness scripts so the harness does not complete prematurely; do not `drain_jobs()` between loading harness and test files.
- **Abort path ownership**: `Transfer` is deleted by `curl_http::Client::on_multi_messages` after `finish()`. The JS abort path must erase the transfer from `Host::fetch_transfers` before deleting it to avoid double-free.
- **Per-request Client**: `fetch_api::async_fetch` constructs a fresh `curl_http::Client` for each call. This keeps `Host` curl-free but is less efficient than a shared multi-handle; it is acceptable for the current workload.
- **Single-threaded assumption per Host**: each `Host`, `Client`, and its `io_context` are not thread-safe; JS and I/O callbacks for that instance run on a single thread. Multiple `Host` instances can exist concurrently, but each should be driven from its own thread or otherwise serialized. Do not call instance methods on a `Host` from a different thread than its event loop.
- **Global registry thread-safety**: the global function registry (`Host::register_global_function`, `FunctionRegistry::register_global_function`) is protected by a `std::shared_mutex` read-write lock, so multiple concurrent `Host` instances can safely register and call global functions. The per-instance registry is not locked; only access it from the owning `Host`'s thread.
- **Host* callback parameter**: registered callbacks can optionally take a `Host*` (or `Host&`) parameter to identify the caller. It does not consume a JS argument, so `call("name", 1, 2)` can invoke a C++ callback `int(Host*, int, int)`. Use `host->id()` as a key when you need per-instance data isolation in shared global functions.
- **Pending ops**: async operations (timers, fetches) increment `pending_ops`. The event loop exits when `pending_ops == 0` and no JS jobs are pending. If a callback forgets to decrement, the loop will hang.
- **HTTP server keep-alive**: the event loop keeps running while `http_active` is true. The HTTP server increments `pending_ops` is NOT done - instead `http_active` flag keeps the loop alive. Call `server.close()` to stop.
- **Streaming uses sync writes**: `write_chunk_sync()` uses blocking `asio::write()`. This is safe for single-threaded event loop but would block in a multi-threaded setup.
- **Response header extraction**: headers are extracted in JS via `__extractResponse()` and passed to C++ as a plain object. All headers are iterated using `Object.keys()` in C++.
- **Hono bundle**: the real Hono v4 is bundled from npm via esbuild. To update, run `.\build-hono.ps1` (requires Node.js).

## C++ code formatting

This project uses [Uncrustify](https://github.com/uncrustify/uncrustify) with the Rust-style configuration in `uncrustify.cfg`.

- After editing any C++ file (`src/` or `tests/`), reformat it before committing.
- VS Code: project-level auto-format on save is disabled for C/C++ in `.vscode/settings.json`; run Uncrustify manually.
- Format a single file:
  ```powershell
  .tools/bin/uncrustify.exe -c uncrustify.cfg -f src/some_file.cpp -o src/some_file.cpp --no-backup
  ```
- Batch format all C++ sources (from the project root):
  ```powershell
  for f in src/*.cpp src/*.hpp tests/*.cpp tests/*.hpp; do
      .tools/bin/uncrustify.exe -c uncrustify.cfg -f $f -o $f --no-backup
  done
  ```

### Downloading Uncrustify

The binary is intentionally kept out of Git. A Windows x64 build can be downloaded from the [Uncrustify GitHub releases](https://github.com/uncrustify/uncrustify/releases) page (look for `uncrustify-X.Y.Z_f-win64.zip`). Extract the `.exe` to `.tools/bin/`; the `.tools/` directory is already listed in `.gitignore` and must not be committed.

### Known limitations

- Long function signatures are split so each parameter is indented one level from the declaration line, not aligned under the opening parenthesis. Multi-line definitions use **Allman braces** — the closing `)` is aligned with the declaration line and `{` is on the next line. This is the closest we can get to Rust style with uncrustify; uncrustify cannot place `) {` on the same line while keeping `)` aligned with the declaration line.
- C++ return types live before the function name, so very long return types (like `async_simple::coro::Lazy<void>`) may end up on their own line.
- Nested function calls can come out slightly off; if a call like `host->spawn_lazy(handle_async_call(...))` formats awkwardly, fix it by hand or split into a temporary variable.

## Where to start

- To understand the runtime: `src/host.hpp`, `src/host.cpp`, `src/main.cpp`.
- To understand fetch / curl integration: `src/fetch.hpp`, `src/fetch.cpp`, `src/curl_http.hpp`, `src/curl_runtime.hpp`, `src/curl_runtime.cpp`, `src/js/fetch.js`.
- To understand JS bindings: `src/qjs.hpp`.
- To understand crypto / Buffer: `src/crypto.hpp`, `src/crypto.cpp`, `src/js/crypto.js`, `src/js/buffer.js`.
- To understand HTTP server: `src/net/http_server.hpp`, `src/net/http_server.cpp`, `src/js/http.js`.
- To understand module loader: `src/qjs_module.cpp`, `src/qjs.hpp` (Runtime class).
- To update WPT results: `docs/wpt-fetch.md`, `docs/wpt-fetch-status.md`, `tests/wpt/manifest.txt`.

---

*Last updated: 2026-07-24. Keep this file in sync with `CMakeLists.txt`, `vcpkg.json`, `docs/*.md`, and the `src/` layout when making structural changes.*
