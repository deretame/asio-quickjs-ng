// Runs official WPT fetch tests + local suites. Network fixtures via Node server.
#include "curl_http.hpp"
#include "fetch.hpp"
#include "host.hpp"
#include "qjs.hpp"

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

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string quote_js(const std::string &s) {
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

Meta parse_meta(std::string_view src) {
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

std::string strip_meta(std::string_view src) {
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

fs::path wpt_root() {
  const char *env = std::getenv("WPT_ROOT");
  if (env && *env) {
    return fs::path(env);
  }
  std::vector<fs::path> candidates = {
      fs::path("third_party/wpt"),
      fs::path("../third_party/wpt"),
      fs::path("../../third_party/wpt"),
      fs::path(ASIO_QJS_SOURCE_DIR) / "third_party" / "wpt",
  };
  for (const auto &c : candidates) {
    if (fs::exists(c / "resources" / "testharness.js")) {
      return fs::weakly_canonical(c);
    }
  }
  return {};
}

fs::path repo_root_from_wpt(const fs::path &wpt) {
  return wpt.parent_path().parent_path();
}

bool eval_path(Host &host, const fs::path &p, bool drain = true) {
  auto code = read_file(p);
  if (code.empty() && !fs::exists(p)) {
    return false;
  }
  return host.eval_source(code, p.generic_string().c_str(), drain);
}

// --- Node fixture server ---
class NodeFixtureServer {
public:
  bool start(const fs::path &repo) {
    script_ = repo / "tests" / "wpt" / "node_test_server.mjs";
    if (!fs::exists(script_)) {
      return false;
    }
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
      return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE in_rd = nullptr, in_wr = nullptr;
    if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) {
      CloseHandle(rd);
      CloseHandle(wr);
      return false;
    }
    SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = in_rd;

    std::wstring cmd = L"node \"" + script_.wstring() + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0,
                             nullptr, repo.wstring().c_str(), &si, &pi);
    CloseHandle(wr);
    CloseHandle(in_rd);
    if (!ok) {
      CloseHandle(rd);
      CloseHandle(in_wr);
      return false;
    }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    out_rd_ = rd;
    in_wr_ = in_wr;

    // Read READY line
    std::string line;
    char ch;
    DWORD n = 0;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      DWORD avail = 0;
      if (!PeekNamedPipe(out_rd_, nullptr, 0, nullptr, &avail, nullptr)) {
        break;
      }
      if (avail == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (!ReadFile(out_rd_, &ch, 1, &n, nullptr) || n != 1) {
        break;
      }
      if (ch == '\n') {
        break;
      }
      if (ch != '\r') {
        line.push_back(ch);
      }
    }
    if (line.rfind("READY ", 0) != 0) {
      stop();
      return false;
    }
    origin_ = line.substr(6);
    while (!origin_.empty() &&
           (origin_.back() == ' ' || origin_.back() == '\r')) {
      origin_.pop_back();
    }
    return !origin_.empty();
#else
    int out_pipe[2];
    int in_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(in_pipe) != 0) {
      return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(out_pipe[1], STDERR_FILENO);
      dup2(in_pipe[0], STDIN_FILENO);
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(in_pipe[0]);
      close(in_pipe[1]);
      auto script = script_.string();
      execlp("node", "node", script.c_str(), static_cast<char *>(nullptr));
      _exit(127);
    }
    close(out_pipe[1]);
    close(in_pipe[0]);
    pid_ = pid;
    out_fd_ = out_pipe[0];
    in_fd_ = in_pipe[1];

    std::string line;
    char ch;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(out_fd_, &rfds);
      timeval tv{0, 50000};
      int r = select(out_fd_ + 1, &rfds, nullptr, nullptr, &tv);
      if (r <= 0) {
        continue;
      }
      if (read(out_fd_, &ch, 1) != 1) {
        break;
      }
      if (ch == '\n') {
        break;
      }
      if (ch != '\r') {
        line.push_back(ch);
      }
    }
    if (line.rfind("READY ", 0) != 0) {
      stop();
      return false;
    }
    origin_ = line.substr(6);
    return !origin_.empty();
#endif
  }

  void stop() {
#ifdef _WIN32
    if (in_wr_) {
      DWORD w = 0;
      const char *q = "quit\n";
      WriteFile(in_wr_, q, 5, &w, nullptr);
      CloseHandle(in_wr_);
      in_wr_ = nullptr;
    }
    if (proc_) {
      if (WaitForSingleObject(proc_, 2000) != WAIT_OBJECT_0) {
        TerminateProcess(proc_, 1);
      }
      CloseHandle(proc_);
      proc_ = nullptr;
    }
    if (out_rd_) {
      CloseHandle(out_rd_);
      out_rd_ = nullptr;
    }
#else
    if (in_fd_ >= 0) {
      const char *q = "quit\n";
      (void)write(in_fd_, q, 5);
      close(in_fd_);
      in_fd_ = -1;
    }
    if (pid_ > 0) {
      int status = 0;
      for (int i = 0; i < 20; ++i) {
        if (waitpid(pid_, &status, WNOHANG) == pid_) {
          pid_ = -1;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (pid_ > 0) {
        kill(pid_, SIGKILL);
        waitpid(pid_, &status, 0);
        pid_ = -1;
      }
    }
    if (out_fd_ >= 0) {
      close(out_fd_);
      out_fd_ = -1;
    }
#endif
    origin_.clear();
  }

  const std::string &origin() const { return origin_; }

private:
  fs::path script_;
  std::string origin_;
#ifdef _WIN32
  HANDLE proc_ = nullptr;
  HANDLE out_rd_ = nullptr;
  HANDLE in_wr_ = nullptr;
#else
  pid_t pid_ = -1;
  int out_fd_ = -1;
  int in_fd_ = -1;
#endif
};

struct FileResult {
  std::string path;
  int passed = 0;
  int failed = 0;
  int skipped = 0;
  std::vector<std::string> failures;
  std::string harness_error;
};

bool needs_fixture(const std::string &entry) {
  return entry.find("abort-network") != std::string::npos ||
         entry.find("network") != std::string::npos;
}

FileResult run_wpt_file(Host &host, const fs::path &wpt, const fs::path &repo,
                        const std::string &entry,
                        const std::string &fixture_origin) {
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
      !eval_path(host, repo / "tests" / "wpt" / "testharnessreport.js",
                 false)) {
    fr.harness_error = "failed loading testharness bootstrap";
    fr.failed = 1;
    return fr;
  }

  if (!fixture_origin.empty()) {
    host.eval_source("globalThis.__TEST_ORIGIN = " + quote_js(fixture_origin) +
                         ";",
                     "fixture_origin.js", false);
  }

  if (!meta.title.empty()) {
    host.eval_source("var META_TITLE = " + quote_js(meta.title) + ";",
                     "meta_title.js", false);
  }

  for (const auto &s : meta.scripts) {
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

  if (!host.eval_source(R"JS(
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

std::vector<std::string> load_manifest(const fs::path &repo) {
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

} // namespace

class WptFetch : public ::testing::Test {
protected:
  void SetUp() override {
    wpt_ = wpt_root();
    ASSERT_FALSE(wpt_.empty())
        << "third_party/wpt not found; clone WPT sparse checkout";
    repo_ = repo_root_from_wpt(wpt_);
    ASSERT_TRUE(node_.start(repo_))
        << "failed to start node test server (is node on PATH?)";
  }

  void TearDown() override { node_.stop(); }

  bool setup_host(Host &host) {
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

  for (const auto &rel : files) {
    Host host;
    ASSERT_TRUE(setup_host(host));
    const std::string &origin =
        needs_fixture(rel) || rel.find("external") != std::string::npos
            ? node_.origin()
            : node_.origin(); // always inject; harmless for offline suites
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
    for (const auto &f : fr.failures) {
      summary << f << "\n";
    }

    EXPECT_TRUE(fr.harness_error.empty()) << rel << ": " << fr.harness_error;
    EXPECT_EQ(fr.failed, 0) << rel << " failures:\n"
                            << (fr.failures.empty() ? "" : fr.failures.front());
  }

  RecordProperty("wpt_pass", std::to_string(total_pass));
  RecordProperty("wpt_fail", std::to_string(total_fail));
  RecordProperty("wpt_skip", std::to_string(total_skip));
  spdlog::info("fixture {}\n{}\nTOTAL pass={} fail={} skip={}",
               node_.origin(), summary.str(), total_pass, total_fail,
               total_skip);
  EXPECT_GT(total_pass, 0);
  EXPECT_EQ(total_fail, 0);
}
