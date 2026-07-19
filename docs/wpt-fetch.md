# 官方 WPT Fetch 测试

## 来源

稀疏检出官方仓库（不全量 clone）：

```text
third_party/wpt/   <- https://github.com/web-platform-tests/wpt
  fetch/api/
  resources/testharness.js
```

更新：

```powershell
cd third_party/wpt
git pull --ff-only
```

若尚未检出：

```powershell
git clone --filter=blob:none --sparse --depth 1 `
  https://github.com/web-platform-tests/wpt.git third_party/wpt
cd third_party/wpt
git sparse-checkout set fetch/api resources common
```

## 如何跑

需要 **Node.js** 在 PATH 上（可控 HTTP fixture，替代 wptserve 子集）。

```powershell
cmake --build build --target test_wpt_fetch
.\build\test_wpt_fetch.exe
# 或
ctest --test-dir build -R WptFetch --output-on-failure
```

Runner 会启动 `tests/wpt/node_test_server.mjs`，注入 `globalThis.__TEST_ORIGIN`。

可选：`WPT_ROOT` 指向 wpt 根目录。

## Runner

| 文件 | 作用 |
|------|------|
| `tests/wpt/shell_bootstrap.js` | `self` + `GLOBAL`，走 testharness **Shell** 环境 |
| `tests/wpt/testharnessreport.js` | `add_completion_callback` → `__WPT_RESULTS__` |
| `tests/wpt/manifest.txt` | 要跑的官方用例列表 |
| `tests/test_wpt_fetch.cpp` | 读 manifest、解析 `// META:`、加载依赖、汇总 gtest |

注意：加载 testharness / 用例时必须 **不要** 中途 `drain_jobs`，否则 `all_loaded` 过早为 true，会在第一个 subtest 后就 `complete()`。

## 范围（刻意不做 100%）

**已纳入：**

| 类别 | 来源 |
|------|------|
| Headers / Request / Response 构造 | 官方 `fetch/api/{headers,request,response}/` |
| `about:` / `data:` scheme | 官方 `fetch/api/basic/scheme-*.any.js` |
| historical API 删除项 | 官方 `fetch/api/basic/historical.any.js` |
| AbortController / pre-abort fetch | 本地 `tests/wpt/local/abort-basic.any.js`（WPT 风格；官方 `abort/general` 依赖 wptserve） |
| 真实 HTTP abort | `tests/test_fetch.cpp`（loopback） |

**仍排除：**

- Cookie / `Set-Cookie` / credentials
- CORS 全量、官方 `abort/general`（要 stash/跨域资源）
- Blob / FormData / ReadableStream body
- multi-globals、service worker

扩面：改 `tests/wpt/manifest.txt`（`local:foo.any.js` 表示 `tests/wpt/local/foo.any.js`）。

## 本地网络 smoke

与 WPT 并行，仍保留 `tests/test_fetch.cpp`（loopback HTTP）覆盖真实 `fetch()` GET/POST/404/redirect。
