# Fetch / WPT 测试过程与结果

> 最近全量结果：**TOTAL pass=343 fail=0 skip=0**（2026-07-19 本机跑通）  
> 清单来源：`tests/wpt/manifest.txt`  
> Runner：`tests/test_wpt_fetch.cpp` + 官方 `testharness.js`（Shell 环境）

---

## 1. 怎么跑

```powershell
# 需要：CMake 构建产物 + PATH 上有 node（网络 fixture）
cmake --build build --target test_wpt_fetch
.\build\test_wpt_fetch.exe

# 或
ctest --test-dir build -R WptFetch --output-on-failure
```

流程：

1. 启动 Node fixture：`tests/wpt/node_test_server.mjs` → 打印 `READY http://127.0.0.1:<port>`
2. 注入 `globalThis.__TEST_ORIGIN`
3. 按 `manifest.txt` 逐文件：bootstrap → testharness → 用例 → 汇总 `__WPT_RESULTS__`
4. 退出时关掉 Node 进程

可选环境变量：`WPT_ROOT` 指向 `third_party/wpt`。

WPT 稀疏检出（若还没有）：

```powershell
git clone --filter=blob:none --sparse --depth 1 `
  https://github.com/web-platform-tests/wpt.git third_party/wpt
cd third_party/wpt
git sparse-checkout set fetch/api resources common
```

---

## 2. 已通过（清单内，全部 fail=0）

路径前缀：`third_party/wpt/`；`local:` = `tests/wpt/local/`。

### 2.1 官方 Headers

| 文件 | pass | 覆盖点 |
|------|-----:|--------|
| `fetch/api/headers/headers-casing.any.js` | 4 | 大小写不敏感 |
| `fetch/api/headers/headers-combine.any.js` | 6 | 同名合并 |
| `fetch/api/headers/headers-errors.any.js` | 18 | 非法名/值 |
| `fetch/api/headers/headers-normalize.any.js` | 3 | 空白规范化 |
| `fetch/api/headers/headers-structure.any.js` | 8 | 结构/方法存在性 |

### 2.2 官方 Response

| 文件 | pass | 覆盖点 |
|------|-----:|--------|
| `fetch/api/response/response-init-001.any.js` | 9 | 默认构造属性 |
| `fetch/api/response/response-static-error.any.js` | 2 | `Response.error()` |
| `fetch/api/response/response-static-json.any.js` | 16 | `Response.json()` |
| `fetch/api/response/response-static-redirect.any.js` | 11 | `Response.redirect()` |
| `fetch/api/response/response-error.any.js` | 10 | 非法 status 等 |
| `fetch/api/response/response-consume-empty.any.js` | 14 | 空 body 消费 / bodyUsed |
| `fetch/api/response/response-init-contenttype.any.js` | 18 | 默认 Content-Type |

### 2.3 官方 Request

| 文件 | pass | 覆盖点 |
|------|-----:|--------|
| `fetch/api/request/request-error.any.js` | 22 | 构造错误路径 |
| `fetch/api/request/request-headers.any.js` | 61 | 禁止请求头 / no-cors 头 |
| `fetch/api/request/forbidden-method.any.js` | 6 | CONNECT/TRACE/TRACK |
| `fetch/api/request/request-structure.any.js` | 24 | 属性/只读/方法表面 |
| `fetch/api/request/request-consume-empty.any.js` | 14 | 空 body 消费 |
| `fetch/api/request/request-disturbed.any.js` | 9 | bodyUsed / 再构造 |
| `fetch/api/request/request-init-contenttype.any.js` | 18 | 默认 Content-Type |

### 2.4 官方 basic / scheme

| 文件 | pass | 覆盖点 |
|------|-----:|--------|
| `fetch/api/basic/historical.any.js` | 3 | 已删除 API 不存在 |
| `fetch/api/basic/scheme-about.any.js` | 7 | `about:` 网络错误 |
| `fetch/api/basic/scheme-data.any.js` | 8 | `data:` 本地解析 |

### 2.5 本地用例（WPT 风格 + Node fixture）

| 文件 | pass | 覆盖点 |
|------|-----:|--------|
| `local:abort-basic.any.js` | 10 | AbortController / 起飞前 abort / data: |
| `local:abort-network.any.js` | 23 | 对齐官方 abort/general 的可离线子集（含飞行中 abort、body 读 abort） |
| `local:http-basic.any.js` | 14 | GET/HEAD/PUT/DELETE/PATCH、404/500、redirect、并发、大 body、`redirect:error` |
| `local:response-guard.any.js` | 3 | 网络响应头不可变、clone |
| `local:external-smoke.any.js` | 2 | `https://example.com` 冒烟（离线则软通过） |

### 合计

```text
TOTAL pass=343 fail=0 skip=0
```

清单内**当前没有失败项**。若某次运行出现 fail，以 `test_wpt_fetch` 终端输出为准，并回查实现回归。

---

## 3. 未纳入清单（未测 / 故意跳过）

下列存在于官方 `fetch/api/`，**未加入 `manifest.txt`**，因此**不算“测过”**。

### 3.1 曾尝试或评估后推迟

| 文件/主题 | 原因 |
|-----------|------|
| `headers/headers-basic.any.js` | 迭代过程中增删头的 **live iterator** 语义未实现；非 generator 迭代器已部分做，mutation 用例仍 fail |
| `headers/headers-record.any.js` | Proxy / getOwnPropertyDescriptor 顺序 |
| `abort/general.any.js`（全文） | 依赖 wptserve：`stash-put.py`、慢流、`get-host-info` **跨域** |
| `abort/request.any.js` 等 | 重 FormData/Blob/stream 与资源路径 |
| `response-clone.any.js`（全文） | 需 `body.getReader()` 真流 + `trickle.py` |
| `response-init-002` / 大量 `response-stream-*` | ReadableStream 真实现 / pipe |
| `request-init-002` 全文 | Blob 细节 + stream 为主 |
| `headers-no-cors.any.js` | 要拉 `not-cors-safelisted.json`（可本地化，未做） |

### 3.2 浏览器 / 规范边角（项目非目标）

- CORS / opaque / `no-cors` 完整网络行为  
- Cookie / `Set-Cookie` / credentials 模式  
- Service Worker、multi-global、`Window` 专用  
- Cache mode 全矩阵（`request-cache-*.any.js`）  
- 完整 bad-port 列表联网探测  
- TLS 客户端证书、HTTP/2 特有行为  

### 3.3 入站能力（与 fetch 出站无关）

- TCP/HTTP **服务器** API：见 `docs/server-runtime.md`，**尚未实现、无 WPT**

---

## 4. 实现与测试的对应关系（摘要）

| 能力 | 主要代码 | 主要测试 |
|------|----------|----------|
| Headers | `src/js/headers.js` | headers-*、request-headers |
| Request/Response | `src/js/request.js` `response.js` | request-* / response-* |
| fetch + abort | `src/js/fetch.js` `abort.js` + C++ abort | abort-*、http-basic |
| data:/about: | `fetch.js` | scheme-* |
| HTTP 传输 | `curl_http.hpp` `fetch.cpp` | Node fixture 网络套件 |
| Blob/FormData/URL/RS 桩 | `src/js/body_polyfill.js` | contenttype / consume-empty / disturbed |

---

## 5. 如何更新本表

1. 改 `tests/wpt/manifest.txt`（加/删路径）  
2. 跑 `.\build\test_wpt_fetch.exe`  
3. 把终端里每文件的 `pass=/fail=` 与 **TOTAL** 贴回本节第 2/3 章  
4. 新增 fail 时：在「未纳入」或单独「已知失败」小节记原因  

---

## 6. 相关文件

| 路径 | 作用 |
|------|------|
| `tests/wpt/manifest.txt` | 跑哪些文件（唯一清单） |
| `tests/test_wpt_fetch.cpp` | C++ runner |
| `tests/wpt/shell_bootstrap.js` | Shell 全局 |
| `tests/wpt/testharnessreport.js` | 结果收集 |
| `tests/wpt/node_test_server.mjs` | 可控 HTTP |
| `tests/wpt/local/*.any.js` | 本地 WPT 风格用例 |
| `docs/wpt-fetch.md` | 环境与范围说明（偏操作） |
| `docs/wpt-fetch-status.md` | **本文：通过/未测对照** |
