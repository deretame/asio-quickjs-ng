// Runs official WPT fetch tests + local suites. Network fixtures via Node
// server.
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "curl_http.hpp"
#include "fetch.hpp"
#include "host.hpp"
#include "node_fixture.hpp"
#include "qjs.hpp"

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p)
{
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string quote_js(const std::string& s)
{
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('"');
  return out;
}

struct Meta {
  std::string title;
  std::vector<std::string> scripts;
};

Meta parse_meta(std::string_view src)
{
  Meta m;
  std::size_t pos = 0;
  while (pos < src.size()) {
    auto eol = src.find('\n', pos);
    if (eol == std::string_view::npos) {
      eol = src.size();
    }
    auto line = src.substr(pos, eol - pos);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    pos = eol + (eol < src.size() ? 1 : 0);

    if (line.rfind("// META:", 0) != 0) {
      if (line.empty() || line.rfind("//", 0) == 0) {
        continue;
      }
      break;
    }
    auto rest = line.substr(8);
    while (!rest.empty() && rest.front() == ' ') {
      rest.remove_prefix(1);
    }
    if (rest.rfind("title=", 0) == 0) {
      m.title = std::string(rest.substr(6));
    } else if (rest.rfind("script=", 0) == 0) {
      m.scripts.emplace_back(rest.substr(7));
    }
  }
  return m;
}

std::string strip_meta(std::string_view src)
{
  std::string out;
  std::size_t pos = 0;
  bool in_meta = true;
  while (pos < src.size()) {
    auto eol = src.find('\n', pos);
    if (eol == std::string_view::npos) {
      eol = src.size();
    }
    auto line = src.substr(pos, eol - pos);
    pos = eol + (eol < src.size() ? 1 : 0);
    if (in_meta) {
      auto t = line;
      if (!t.empty() && t.back() == '\r') {
        t.remove_suffix(1);
      }
      if (t.rfind("// META:", 0) == 0 || t.empty() || t.rfind("//", 0) == 0) {
        continue;
      }
      in_meta = false;
    }
    out.append(line);
    out.push_back('\n');
  }
  return out;
}

fs::path wpt_root()
{
  const char* env = std::getenv("WPT_ROOT");
  if (env && *env) {
    return fs::path(env);
  }
  std::vector<fs::path> candidates = {
    fs::path("third_party/wpt"),
    fs::path("../third_party/wpt"),
    fs::path("../../third_party/wpt"),
    fs::path(ASIO_QJS_SOURCE_DIR) / "third_party" / "wpt",
  };
  for (const auto& c : candidates) {
    if (fs::exists(c / "resources" / "testharness.js")) {
      return fs::weakly_canonical(c);
    }
  }
  return {};
}

fs::path repo_root_from_wpt(const fs::path& wpt)
{
  return wpt.parent_path().parent_path();
}

bool eval_path(Host& host, const fs::path& p, bool drain = true)
{
  auto code = read_file(p);
  if (code.empty() && !fs::exists(p)) {
    return false;
  }
  return host.eval_source(code, p.generic_string().c_str(), drain);
}

struct FileResult {
  std::string path;
  int passed = 0;
  int failed = 0;
  int skipped = 0;
  std::vector<std::string> failures;
  std::string harness_error;
};

bool needs_fixture(const std::string& entry)
{
  return entry.find("abort-network") != std::string::npos ||
         entry.find("network") != std::string::npos;
}

FileResult run_wpt_file(
  Host& host,
  const fs::path& wpt,
  const fs::path& repo,
  const std::string& entry,
  const std::string& fixture_origin
)
{
  FileResult fr;
  fr.path = entry;

  fs::path abs;
  if (entry.rfind("local:", 0) == 0) {
    abs = repo / "tests" / "wpt" / "local" / entry.substr(6);
  } else {
    abs = wpt / entry;
  }
  auto src = read_file(abs);
  if (src.empty()) {
    fr.harness_error = "cannot read " + abs.string();
    fr.failed = 1;
    return fr;
  }

  auto meta = parse_meta(src);
  const fs::path dir = abs.parent_path();

  if (!eval_path(host, repo / "tests" / "wpt" / "shell_bootstrap.js", false) ||
    !eval_path(host, wpt / "resources" / "testharness.js", false) ||
    !eval_path(
    host,
    repo / "tests" / "wpt" / "testharnessreport.js",
    false)) {
    fr.harness_error = "failed loading testharness bootstrap";
    fr.failed = 1;
    return fr;
  }

  if (!fixture_origin.empty()) {
    host.eval_source(
      "globalThis.__TEST_ORIGIN = " + quote_js(fixture_origin) + ";",
      "fixture_origin.js",
      false);
  }

  if (!meta.title.empty()) {
    host.eval_source(
      "var META_TITLE = " + quote_js(meta.title) + ";",
      "meta_title.js",
      false);
  }

  for (const auto& s : meta.scripts) {
    fs::path sp = dir / s;
    if (!fs::exists(sp)) {
      sp = wpt / s;
    }
    if (!eval_path(host, sp, false)) {
      fr.harness_error = "failed loading META script " + s;
      fr.failed = 1;
      return fr;
    }
  }

  auto body = strip_meta(src);
  if (!host.eval_source(body, abs.generic_string().c_str(), false)) {
    fr.harness_error = "eval failed";
    fr.failed = 1;
    return fr;
  }

  host.drain_jobs();

  // Drive event loop until harness completion (network tests need time).
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    host.drain_jobs();
    if (host.ioc.stopped()) {
      host.ioc.restart();
    }
    auto done = host.global().get("__WPT_DONE__");
    if (JS_ToBool(host.js_raw(), done.raw())) {
      break;
    }
    if (host.pending_ops > 0) {
      host.ioc.run_one_for(std::chrono::milliseconds(50));
    } else {
      host.ioc.poll();
      host.drain_jobs();
      auto done2 = host.global().get("__WPT_DONE__");
      if (JS_ToBool(host.js_raw(), done2.raw())) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  auto results = host.global().get("__WPT_RESULTS__");
  if (JS_IsNull(results.raw()) || JS_IsUndefined(results.raw())) {
    fr.harness_error = "no __WPT_RESULTS__ (harness did not complete)";
    fr.failed = 1;
    return fr;
  }

  if (!host.eval_source(
    R"JS(
    (function () {
      var r = globalThis.__WPT_RESULTS__;
      globalThis.__WPT_PASS__ = 0;
      globalThis.__WPT_FAIL__ = 0;
      globalThis.__WPT_SKIP__ = 0;
      globalThis.__WPT_FAIL_MSG__ = "";
      if (!r) return;
      if (r.harness && r.harness !== 0) {
        globalThis.__WPT_FAIL__ = 1;
        globalThis.__WPT_FAIL_MSG__ = "harness: " + r.harness_message;
        return;
      }
      for (var i = 0; i < r.tests.length; ++i) {
        var t = r.tests[i];
        if (t.status === 0) globalThis.__WPT_PASS__++;
        else if (t.status === 4 || t.status === 3) globalThis.__WPT_SKIP__++;
        else {
          globalThis.__WPT_FAIL__++;
          globalThis.__WPT_FAIL_MSG__ += t.name + ": " + t.message + "\n";
        }
      }
    })();
  )JS",
    "parse_results.js")) {
    fr.harness_error = "parse results failed";
    fr.failed = 1;
    return fr;
  }

  int32_t pass = 0, fail = 0, skip = 0;
  host.global().get("__WPT_PASS__").to_int32(pass);
  host.global().get("__WPT_FAIL__").to_int32(fail);
  host.global().get("__WPT_SKIP__").to_int32(skip);
  fr.passed = pass;
  fr.failed = fail;
  fr.skipped = skip;
  auto msg = host.global().get("__WPT_FAIL_MSG__").to_std_string();
  if (msg && !msg->empty()) {
    fr.failures.push_back(*msg);
  }
  return fr;
}

std::vector<std::string> load_manifest(const fs::path& repo)
{
  auto text = read_file(repo / "tests" / "wpt" / "manifest.txt");
  std::vector<std::string> out;
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    out.push_back(line);
  }
  return out;
}

}  // namespace

class WptFetch : public ::testing::Test {
 protected:
  void SetUp() override
  {
    wpt_ = wpt_root();
    ASSERT_FALSE(wpt_.empty())
      << "third_party/wpt not found; clone WPT sparse checkout";
    repo_ = repo_root_from_wpt(wpt_);
    auto script = repo_ / "tests" / "wpt" / "node_test_server.mjs";
    ASSERT_TRUE(node_.start(script, repo_))
      << "failed to start node test server (is node on PATH?)";
  }

  void TearDown() override { node_.stop(); }

  bool setup_host(Host& host)
  {
    return host && host.install_runtime() && fetch_api::install(host);
  }

  fs::path wpt_;
  fs::path repo_;
  NodeFixtureServer node_;
};

TEST_F(WptFetch, OfficialManifest) {
  auto files = load_manifest(repo_);
  ASSERT_FALSE(files.empty());

  int total_pass = 0, total_fail = 0, total_skip = 0;
  std::ostringstream summary;

  for (const auto& rel : files) {
    Host host;
    ASSERT_TRUE(setup_host(host));
    const std::string& origin =
      needs_fixture(rel) || rel.find("external") != std::string::npos?
      node_.origin():
      node_.origin();          // always inject; harmless for offline suites
    auto fr = run_wpt_file(host, wpt_, repo_, rel, origin);
    host.shutdown();

    total_pass += fr.passed;
    total_fail += fr.failed;
    total_skip += fr.skipped;

    summary << rel << "  pass=" << fr.passed << " fail=" << fr.failed
            << " skip=" << fr.skipped << "\n";
    if (!fr.harness_error.empty()) {
      summary << "  HARNESS: " << fr.harness_error << "\n";
    }
    for (const auto& f : fr.failures) {
      summary << f << "\n";
    }

    EXPECT_TRUE(fr.harness_error.empty()) << rel << ": " << fr.harness_error;
    EXPECT_EQ(fr.failed, 0) << rel << " failures:\n"
                            << (fr.failures.empty() ? "" : fr.failures.front());
  }

  RecordProperty("wpt_pass", std::to_string(total_pass));
  RecordProperty("wpt_fail", std::to_string(total_fail));
  RecordProperty("wpt_skip", std::to_string(total_skip));
  spdlog::info(
    "fixture {}\n{}\nTOTAL pass={} fail={} skip={}",
    node_.origin(),
    summary.str(),
    total_pass,
    total_fail,
    total_skip);
  EXPECT_GT(total_pass, 0);
  EXPECT_EQ(total_fail, 0);
}
