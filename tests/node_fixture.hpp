#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// Starts a Node.js HTTP server as a child process for local/WPT-style tests.
// Reads the READY line from stdout to obtain the server origin.
class NodeFixtureServer {
public:
~NodeFixtureServer() { stop(); }

bool start(const fs::path& script, const fs::path& working_dir = {})
{
  script_ = script;
  if (!fs::exists(script_)) {
    return false;
  }
  fs::path cwd = working_dir.empty() ? script_.parent_path() : working_dir;

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
  BOOL ok = CreateProcessW(
    nullptr,
    buf.data(),
    nullptr,
    nullptr,
    TRUE,
    0,
    nullptr,
    cwd.wstring().c_str(),
    &si,
    &pi);
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

  if (!read_ready_line()) {
    stop();
    return false;
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
    auto dir = cwd.string();
    chdir(dir.c_str());
    execlp("node", "node", script.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  close(out_pipe[1]);
  close(in_pipe[0]);
  pid_ = pid;
  out_fd_ = out_pipe[0];
  in_fd_ = in_pipe[1];

  if (!read_ready_line()) {
    stop();
    return false;
  }
  return !origin_.empty();

#endif
}

void stop()
{
#ifdef _WIN32
  if (in_wr_) {
    DWORD w = 0;
    const char* q = "quit\n";
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
    const char* q = "quit\n";
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

const std::string& origin() const { return origin_; }

private:
bool read_ready_line()
{
  std::string line;
  char ch;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
    DWORD avail = 0;
    if (!PeekNamedPipe(out_rd_, nullptr, 0, nullptr, &avail, nullptr)) {
      break;
    }
    if (avail == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    DWORD n = 0;
    if (!ReadFile(out_rd_, &ch, 1, &n, nullptr) || n != 1) {
      break;
    }
#else
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
#endif
    if (ch == '\n') {
      break;
    }
    if (ch != '\r') {
      line.push_back(ch);
    }
  }
  if (line.rfind("READY ", 0) != 0) {
    return false;
  }
  origin_ = line.substr(6);
  while (!origin_.empty() &&
    (origin_.back() == ' ' || origin_.back() == '\r')) {
    origin_.pop_back();
  }
  return !origin_.empty();
}

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
