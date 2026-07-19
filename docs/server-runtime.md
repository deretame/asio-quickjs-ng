# C++ 服务器运行时设计

> 目标：C++ 提供异步 IO 与少量原生 API，业务主要用 JS 编写，在单（或多）事件循环上实现高并发。  
> 非目标：完整兼容 Node.js / 让 Express 原样 `require` 跑起来。

## 1. 定位

| 要做 | 不做 |
|------|------|
| 自研 runtime：暴露精简、稳定的 JS API | npm 生态无缝 / Express 二进制兼容 |
| 事件驱动 + 每连接协程，支撑高并发 | 在 C++ 里写业务路由与中间件 |
| `fetch` / timer / TCP /（可选）HTTP 入站 | 一次性实现全量 `net`/`http`/`stream`/`tls` |

一句话：

> **C++ = 异步 IO 运行时 + 薄绑定；JS = 业务；高并发来自事件循环，不来自“兼容 Node”。**

## 2. 分层

```
┌──────────────────────────────────────────┐
│  JS 业务（路由、逻辑、协议编排）            │
├──────────────────────────────────────────┤
│  Bindings：net / http / timer / fetch …  │  薄：类型转换 + 生命周期
├──────────────────────────────────────────┤
│  Runtime：Host / Executor / 事件循环       │  已有 host.*
├──────────────────────────────────────────┤
│  IO：asio（socket/timer）+ curl（出站）    │
└──────────────────────────────────────────┘
```

职责边界：

- **Host**：`io_context`、QuickJS、`pending_ops`、`spawn_lazy`、`run_loop`、全局安装入口。
- **IO 模块**：只关心连接、缓冲、超时、取消、错误码。
- **Bindings**：把 C++ 对象暴露成 JS 对象/函数；不写业务。
- **JS**：`listen`、处理 `connection`/`request`、调用 `write`/`end`。

## 3. 与现有代码的关系

当前已具备：

| 组件 | 路径 | 作用 |
|------|------|------|
| Host | `src/host.*` | 事件循环、curl multi 驱动、runtime 绑定 |
| Fetch（出站） | `src/fetch.*` | `async_fetch` / `__nativeFetch` + JS polyfill |
| curl 封装 | `src/curl_http.hpp` | easy/multi/Transfer |
| 执行器 | `src/asio_executor.hpp` | async_simple → asio |
| Channel | `src/channel.hpp` | 协程间 mpsc/oneshot（测试与多 VM 通信） |

建议增量目录：

```text
src/
  host.*
  fetch.*
  net/
    tcp_socket.*
    tcp_server.*
    http_server.*      # 二期
  bind/
    net_js.*           # QuickJS 绑定
tests/
  test_tcp.cpp
  test_http.cpp
docs/
  server-runtime.md    # 本文
```

## 4. 核心并发模型

### 4.1 事件循环

- 默认 **单线程** `asio::io_context{1}`（与现 Host 一致），逻辑简单、无锁。
- 循环条件沿用：`pending_ops > 0 || JS job pending`，再 `run_one`。
- 原生回调里 **禁止** 长时间同步阻塞（包括同步 DNS、大文件读）。

### 4.2 连接生命周期

```text
accept
  → Session = shared_ptr<TcpSocket>
  → 读协程（Lazy）循环 async_read_some
  → 数据就绪 → 投递 JS on("data") / HTTP 解析后 on("request")
  → JS write → C++ 写队列 → async_write
  → end/close/error
  → 取消 pending 操作
  → 释放 JS 句柄（finalizer / detach）
  → shared_ptr 归零
```

要点：

1. 每个 socket 使用 `enable_shared_from_this`；异步回调只抓 `shared_ptr` 或 `weak_ptr`。
2. JS 侧持有 opaque 指针时，C++ 析构必须 **detach**，避免悬空。
3. 读、写、定时器都计入生命周期（`pending_ops` 或 session 引用），避免 loop 过早退出。

### 4.3 多核（后期）

第一版不需要。以后可：

- N 线程 × N 个 `io_context`；
- `accept` 后把 socket 挂到 worker；
- 或同 ioc + 多 strand（通常不如多 ioc 简单）。

## 5. JS API 形状（目标，非 Node 全兼容）

### 5.1 一期：TCP

```js
const server = await listen({ host: "0.0.0.0", port: 8080 });

server.on("connection", (socket) => {
  socket.on("data", (buf) => {
    socket.write(buf); // echo
  });
  socket.on("close", () => {});
  socket.on("error", (err) => {});
});

// 客户端
const sock = await connect({ host: "127.0.0.1", port: 8080 });
sock.write("hello");
```

C++ 侧最小表面：

| 类型 | 能力 |
|------|------|
| `TcpServer` | `listen` / `close` / `on_connection` |
| `TcpSocket` | `read` 循环、`write` 队列、`end`、`destroy`、基本事件 |

### 5.2 二期：HTTP 入站（自研，不是 http 模块兼容）

```js
const server = createHttpServer((req, res) => {
  res.writeHead(200, { "content-type": "text/plain" });
  res.end("ok");
});
await server.listen(8080);
```

- 解析可用 **llhttp** / **picohttpparser**，不要手写完整 HTTP/1.1。
- `req`/`res` 是绑定对象，不是 Node 的 `IncomingMessage` 全仿真。
- JS 层可再包一层 **mini-express**（纯 JS：`app.get` / `use`）。

### 5.3 已有：出站与定时器

- `fetch` / `__nativeFetch`（`fetch.*`）
- `setTimeout`（`host` runtime）

## 6. Bindings 约定

1. **同步原生函数只做投递**：创建任务、返回 Promise / 注册回调，立刻返回。
2. **字符串 / 二进制**：先支持 `string` 与 `Uint8Array`；完整 `Buffer` 兼容可后置。
3. **事件**：轻量 `EventEmitter` 风格即可（`on`/`off`/`emit`），可在 JS polyfill 或 C++ 各做一部分。
4. **错误**：统一 `{ code, message }` 或 Error 对象；socket 错误不把进程打崩。
5. **背压（二期）**：写队列超限时对 JS 发出 `drain` / 让 `write` 返回 false；一期可固定高水位简化。

## 7. C++ 模块职责草案

### `net/tcp_socket`

- 封装 `asio::ip::tcp::socket`
- `Lazy`/`async_` 读循环与写队列
- 状态：`open` / `closing` / `closed`
- 向 binding 层提供：`start_read`、`write`、`close`

### `net/tcp_server`

- `asio::ip::tcp::acceptor`
- accept 循环 → 创建 `TcpSocket` → 回调 binding/JS

### `net/http_server`（二期）

- 在 `TcpSocket` 上跑解析器
- 拼出 `HttpRequest` / `HttpResponse` 绑定

### `bind/net_js`

- `listen` / `connect` / `createHttpServer` 等全局或模块导出
- 对象 finalizer 与 C++ `shared_ptr` 对齐

## 8. 推荐落地顺序

| 阶段 | 交付 | 验收 |
|------|------|------|
| P0 | 文档与目录骨架 | 本文 + 空模块可编译 |
| P1 | `TcpServer` + `TcpSocket` + JS 绑定 | echo；`tests/test_tcp.cpp` |
| P2 | 写队列 + 基本背压/错误事件 | 大包、慢消费者不崩 |
| P3 | 简易 `HttpServer` + JS handler | `curl` 打 `GET /` 200 |
| P4 | 纯 JS mini-router | `app.get` 风格 demo |
| P5 | 多 `io_context` / TLS（按需） | 压测与 HTTPS |

## 9. 测试策略

- **C++ gtest**：accept/echo、连接关闭、错误路径（不依赖真实外网优先）。
- **JS 脚本**：`tests/js/echo_server.js` 一类集成脚本，由 `asio_qjs` 拉起。
- **fetch 测试**：继续用 `tests/test_fetch.cpp`（出站）；入站 HTTP 另建 `test_http`。
- 网络不稳定用例：`GTEST_SKIP` 或仅本地 loopback。

## 10. 明确不在近期范围

- `require('express')` / 完整 Node 内置模块
- 兼容 libuv 语义的全套 `stream.pipeline`
- 多线程共享一个 JS Runtime（QuickJS 默认假定单线程；多线程需多 Context 或加锁，复杂且易踩坑）
- 在 C++ 里实现中间件生态

## 11. 设计原则（备忘）

1. **API 面宁小勿大**——先 echo，再 HTTP，再“像 Express 的 JS 框架”。
2. **单线程跑通再谈多核**。
3. **生命周期比功能更重要**——泄漏的 socket/JS 句柄会拖死 long-running 服务。
4. **业务不进 C++**——C++ 只保证快与稳，产品形态在 JS。
5. **兼容 Node 是单独产品级目标**，与“高并发 JS 业务”解耦。

## 12. 下一步（可执行）

1. 增加 `src/net/tcp_socket.hpp` / `tcp_server.hpp` 骨架与 `asio_qjs_core` 编译接入。  
2. 实现 loopback echo 的 gtest。  
3. 增加 `bind/net_js`：`listen` + `connection`/`data`/`write`。  
4. 用纯 JS 写 echo demo，再考虑 HTTP 解析器依赖。

---

*本文描述架构方向，随实现迭代更新。*
