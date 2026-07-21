# Host 任务调度（Manager + 指定 Host）

> 状态：**MVP 已实现**（`src/job.hpp`、`src/host_manager.hpp/.cpp`、`tests/test_host_manager.cpp`）  
> 依赖：线程安全 channel（`src/channel.hpp`）、`Host`、function registry / `call(name, …)`  
> 目标：任意线程向**指定 Host** 投递任务，完成后用 oneshot 回传；支持软取消。

## 1. 定位

| 要做 | 不做 |
|------|------|
| Manager 创建并持有多个 Host | 全局 work-stealing / 自动选 Host |
| 调用方**指定 `host_id`** 投递任务 | 跨线程 / 跨 Host 直接碰 QuickJS |
| 任务 = 函数名 + Wire 参数 + oneshot 回传 | 管道里出现 `JSValue` / `nlohmann::json` 当「JS 对象」 |
| 软取消：未开始的不再跑 | 打断正在执行的 JS |

一句话：

> **Host 是封闭隔离域；对外只有窄 API 与 channel 搬 Wire；JS 只在目标 Host 线程上跑。**

---

## 2. 铁律：隔离与 JSValue

### 2.1 Host 完全隔离

每个 Host 是独立隔离域，与其它 Host、以及一切非 Host 的 C++ 线程**彻底隔离**。

**私有（外部禁止直接访问）：**

- `JSRuntime` / `JSContext` / 任意 `JSValue`
- 该实例的 `io_context` 线程、定时器、进行中的 fetch、实例级 registry 状态等

**跨边界只允许：**

| 途径 | 用途 |
|------|------|
| Manager 窄 API | `create_host` / `destroy_host` / `submit` / `cancel` 等 |
| channel（mpsc / oneshot） | 只搬运 **Wire**（见 §4），作为 runtime 实现细节 |
| 只读元数据 | 如 `host_id` 列表（不进入 JS 堆） |

业务代码理想上**只碰 Manager API**；底层 `Sender` 不随便暴露给业务乱连。

审查标准：每增加一个可从外部调用的 `Host` 方法，必须问是否破坏隔离。

### 2.2 JSValue 不出 Host

```text
JSValue 的生命周期 ≤ 单个 Host 的线程 + 其 JSContext。
一旦离开该 Host 范围，协议上不得再出现 JSValue。
```

| 位置 | 可否出现 JSValue |
|------|------------------|
| 目标 Host 线程上：decode 参数、`call`、encode 返回值 | 可以 |
| mpsc / oneshot 载荷、Manager、其它线程、其它 Host | **不可以** |

进出边界必须在 Host 上 **decode（入）/ encode（出）** 成 Wire，再 `send`。

### 2.3 `register_global_function` 与隔离

**保留**全局动态注册：给 Host **注入 C++ 能力**，避免每加一个 API 就改一堆绑定代码。

它与隔离的关系：

```text
隔离的是：每个 Host 的 JS 堆、事件循环、进行中的请求
不隔离的是：进程级「名字 → C++ 函数」注册表（能力目录，供 call 查找）
```

| 可以 | 不可以 |
|------|--------|
| 启动期 / Manager 侧 `register_global_function` | 在 global 回调里缓存 `JSValue` |
| Host 内 `call("name")` 命中全局表 | 把 global 表当成跨 Host 共享 JS 状态总线 |
| 回调用 `Host*` / `host->id()` **按实例分桶** C++ 状态 | 假设全局只有一个 Host、或回调可跨线程碰 JS |
| 返回值只在**当前** call 的 Host 上下文创建 | 把返回的 `JSValue` 存到 Host 外 |

约定：

1. 注册发生在任务跑起来之前（或依赖已有 `shared_mutex` 并发注册）。
2. 全局回调须可重入、可被多 Host 并发调用。
3. 需要每 Host 不同的 JS 闭包 → 用**实例 registry**，或 submit 到该 Host 后再注册。
4. `register_global_function` = **插件 / 能力表**，不是共享 JS 世界。

---

## 3. 角色与数据流

```text
调用方（任意线程 / 其它 Host）
        │
        │  submit(host_id, fn_name, wire_args)
        │  → (task_id, oneshot::Receiver<Result>)
        ▼
┌──────────────── Manager ────────────────┐
│  map<host_id, HostSlot>                 │
│  task_id 分配 + 取消表                  │
│  submit / cancel / create_host / …      │
└───────────────┬─────────────────────────┘
                │  mpsc::Sender<Job>.send(job)   // Wire only
                ▼
┌──────────── Host（指定实例）────────────┐
│  本线程 io_context + QuickJS            │
│  消费队列：                             │
│    if cancelled → reply(Cancelled)      │
│    else wire → JSValue → call → Wire    │
│         → reply(Result)                 │
└─────────────────────────────────────────┘
                │
                │  oneshot::Sender<Result>   // Wire only
                ▼
调用方  co_await rx.recv()
```

### 3.1 Manager

- 创建 Host（默认每 Host 一线程跑 `run_loop`，实现阶段再定）。
- 每 Host 一条 **unbounded mpsc&lt;Job&gt;** 任务队列。
- 对外逻辑 API：
  - `create_host(...) -> host_id`
  - `destroy_host(host_id)`
  - `submit(host_id, fn_name, args) -> (task_id, oneshot::Receiver<Result>)`
  - `cancel(task_id) -> bool`
- **不做**自动负载均衡；谁执行 = 调用方选的 `host_id`。

### 3.2 Host 侧消费循环

```text
for (;;) {
  job = co_await queue_rx.recv()
  if (!job) break

  if (job.cancelled) {
    job.reply.send(Cancelled)
    continue
  }

  try {
    js_args = decode_args(ctx, job.args)     // 仅本 Host 线程
    js_ret  = call(job.fn_name, js_args)
    // 若返回 Promise（async function / 显式 Promise），await 后再 encode
    js_ret  = await_js_promise_if_needed(js_ret)
    job.reply.send(encode_result(ctx, js_ret))
  } catch (e) {
    job.reply.send(Err(e))
  }
}
```

同步与异步对调用方透明：`submit` 始终 oneshot 等**最终**结果。JS `async function`、返回 `Promise`、或内部 `await call("asyncCpp", …)` 均可。

**Host 内 IO 并发（协作式，单线程）：** dispatcher **不** `await` 整段 `execute_job`，而是 `spawn` 后立刻 `recv` 下一条。多个 job 可同时挂在 Promise/timer/fetch 上。`max_in_flight`（默认 64）限制同时执行数，满则 dispatcher 等待。这不是多线程进 JS，而是与 Node 类似的事件循环并发。

---

## 4. Wire 参数与返回值

跨边界**不使用** `nlohmann::json` 作为「给 JS 的对象」类型（JS 不认识它）。  
文本统一用 `std::string`（或等价 UTF-8 字节）；二进制用 `std::vector<uint8_t>` 等，管道内 **move 所有权**。

### 4.1 入参：按「交给 JS 的语义」枚举

枚举描述的是 **Host 如何投递到 JS**，不是「内容是不是 JSON 文本」。

| `ArgsKind` | 管道载荷 | Host 交给 JS |
|------------|----------|--------------|
| `None` | 无 | 无参 |
| `String` | `std::string` | **JS string 原样**（即使内容长得像 JSON 也不 parse） |
| `Object` | `std::string`（UTF-8 文本） | **`JSON.parse` → plain Object** |
| `Bytes` | `std::vector<uint8_t>` | `ArrayBuffer` / `Uint8Array`（能 external 移交则移交） |

因此：

- 要对象：`kind=Object` + `'{"a":1}'` → JS 得到 object  
- 要 JSON **原文当字符串**：`kind=String` + `'{"a":1}'` → JS 得到 string，**不混淆**

### 4.2 回传：C++ 只认 Wire

JS 可能返回 object，但 **C++ 看不懂 `JSValue`**，出 Host 前必须 encode：

| JS 返回 | Host 编码为 | C++ 看到 |
|---------|-------------|----------|
| object / array | **`JSON.stringify` → string** | `OkString`（或 `OkObjectText`，payload 仍是 string） |
| string / number / bool | 收成 string（约定格式） | `OkString` |
| ArrayBuffer / TypedArray | bytes（move/必要拷贝） | `OkBytes` |
| undefined / null | 约定空 / `None` | 明确语义 |
| throw | — | `Err(message)` |

```text
ResultKind: OkString | OkBytes | Err | Cancelled
// 若需区分「业务上这是对象文本」：OkObjectText，payload 仍是 string，不强制解析成 json 库类型
```

调用方若要结构化数据，自行 parse string；调度器不强制 `nlohmann::json`。

### 4.3 入参 / 回传不对称但同边界

| | 入参 | 回传 |
|--|------|------|
| 管道 | string / bytes + kind | string / bytes + kind |
| Object | string + `Object` → Host **parse** | JS object → Host **stringify** → string |
| 谁碰 JS | 仅目标 Host | 仅目标 Host |

### 4.4 大数据

- **默认**：`Bytes` / 大 string 经 channel **move**，不必先上对象池。  
- **池 / blob id**：仅当高频复用或只读共享时再加；非 MVP。

---

## 5. Job / Result 结构（逻辑）

```text
Job {
  task_id          // Manager 生成
  fn_name          // string，call 目标名
  args_kind        // None | String | Object | Bytes
  payload_str      // String / Object 时用
  payload_bytes    // Bytes 时用
  reply            // oneshot::Sender<Result>
  cancelled        // shared_ptr<atomic_bool> 或等价，出队时检查
}

Result {
  kind             // OkString | OkBytes | Err | Cancelled
  payload_str / payload_bytes / message
}
```

- `task_id`、`reply` **放进 Job**，与任务一起入队。  
- 调用方持有：`task_id` + `oneshot::Receiver<Result>`。  
- `Job` **只 move、不拷贝**。  
- 队列类型：`mpsc::unbounded<Job>()`（或 `unique_ptr<Job>`）。

`fn_name` 在目标 Host 上解析：实例 registry 和/或 `register_global_function` 全局表（§2.3）。

---

## 6. 取消语义（软取消）

| 任务状态 | `cancel(task_id)` 之后 |
|----------|-------------------------|
| 仍在队列、尚未 `call` | 出队见 flag → **不调 JS**，`reply(Cancelled)` |
| 已在执行 JS | **不打断**，跑完后按真实结果 `Ok*` / `Err` |
| 已完成 / 未知 id | cancel 无效（返回 false） |

- 不必从 mpsc 中间删节点；**出队检查**即可。  
- `task_id` 由 Manager 分配。  
- Host 销毁：close 队列；未完成 job 统一 `Cancelled` 或 `HostGone`（建议独立错误码）。

---

## 7. 与现有组件的关系

| 组件 | 路径 | 用途 |
|------|------|------|
| Channel mpsc | `src/channel.hpp` | 每 Host 任务队列 |
| Channel oneshot | 同上 | 单次完成通知 |
| Host | `src/host.*` | 事件循环、`spawn_lazy`、decode/call/encode |
| Function registry | `src/function_registry.*` | 按名 call；全局表 = 动态能力注入 |
| Host id | `Host::id()` / `__hostID` | Manager 索引与指定投递 |

---

## 8. API 草图（非最终）

### C++ 调用方

```text
auto [task_id, rx] = manager.submit(host_id, "onJob", args_kind, payload);
// manager.cancel(task_id);
auto result = co_await rx.recv();
```

### JS 侧（可选二期）

- `submitTask(hostId, name, …) -> Promise`：底层仍 oneshot + Wire。  
- 或 JS 只提供被 `call` 的业务函数，submit 仅 C++/Manager。

---

## 9. 生命周期与 pending_ops

- in-flight job（已入队未 reply）计入对应 Host 的 `pending_ops`（或等价保活）。  
- reply 或 cancel 短路后递减。  
- `destroy_host`：停 submit → close 队列 → 消费循环退出 → join → 清 cancel 表。

---

## 10. 背压与扩展（非 MVP）

| 项 | MVP | 以后 |
|----|-----|------|
| 队列 | unbounded mpsc | bounded |
| 调度 | 指定 host_id | 可选路由 |
| 大块 | vector move | blob 池 / handle |
| 取消 | 出队检查 | JS 协作式查 flag |
| 多结果 | 无 | Job 内嵌 mpsc 进度通道 |

---

## 11. 实现顺序建议

1. Wire：`ArgsKind` / `ResultKind` + `Job` + 单 Host 消费循环（桩 `call`）。  
2. Manager：`create_host` / `submit` / `cancel`。  
3. 真实 decode / `call` / encode（String / Object / Bytes）。  
4. Host 销毁与 `pending_ops`。  
5. （可选）JS Promise 糖。

---

## 12. 测试要点

- 同线程 / **跨线程** submit + oneshot 回传。  
- 多 Host：只落到指定 `host_id`。  
- `Object` vs `String`：同文不同 kind，JS 分别得到 object 与 string。  
- 回传 object → C++ 侧为 string 文本，无 `JSValue` 泄漏。  
- cancel 在执行前 → `Cancelled` 且未调用 fn。  
- cancel 在执行中 → fn 仍调用一次。  
- destroy Host：无永久挂起的 `recv`。  
- 全局注册函数可被多 Host 并发 `call`，且不共享 `JSValue`。

---

## 13. 非目标

- 入站 TCP/HTTP（见 `docs/server-runtime.md`）。  
- 用调度器替代 function_registry（调度器在其上做跨线程投递）。  
- 取消正在运行的 JS 字节码。  
- 跨 Host 共享 JS 堆或 `JSValue`。

---

## 14. 铁律摘要（实现 / Review 用）

1. **Host 隔离**：外部不得直接操作 Host 内部；交互 = Manager API 或 channel Wire。  
2. **JSValue ≤ 单 Host**：出边界必须先 encode。  
3. **Wire = string/bytes + 语义枚举**：`Object` 与「JSON 原文 string」靠 kind 区分，不靠猜内容。  
4. **回传 object 必 stringify**（或等价文本）：C++ 永不持有返回的 `JSValue`。  
5. **全局函数表 = 能力目录**：可动态注册；禁止当共享 JS 状态；回调按 `Host*` 分桶。  
6. **软取消**：未开跑的不跑；已开跑的跑完。

---

*与 `src/channel.hpp` 线程安全 mpsc/oneshot 配套。实现时若公共 API 有变，请同步本文与 `AGENTS.md`。*
