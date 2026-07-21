#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "channel.hpp"

// Wire types for Host job scheduling (see docs/host-job-scheduler.md).
// No JSValue crosses Host boundaries — only these C++ types.

namespace job {

// How Host should present args to JS (not "is the text JSON?").
enum class ArgsKind {
  None,    // no argument
  String,  // JS string, payload as-is (even if it looks like JSON)
  Object,  // JSON.parse(payload_str) -> plain Object
  Bytes,   // ArrayBuffer / Uint8Array from payload_bytes
};

enum class ResultKind {
  OkString,  // includes stringified objects
  OkBytes,
  Err,
  Cancelled,
};

struct Args {
  ArgsKind kind = ArgsKind::None;
  std::string str;
  std::vector<uint8_t> bytes;

  static Args make_none() { return Args{}; }

  static Args make_string(std::string s)
  {
    Args a;
    a.kind = ArgsKind::String;
    a.str = std::move(s);
    return a;
  }

  static Args make_object_json(std::string json_text)
  {
    Args a;
    a.kind = ArgsKind::Object;
    a.str = std::move(json_text);
    return a;
  }

  static Args make_bytes(std::vector<uint8_t> b)
  {
    Args a;
    a.kind = ArgsKind::Bytes;
    a.bytes = std::move(b);
    return a;
  }

};

struct Result {
  ResultKind kind = ResultKind::Err;
  std::string str;
  std::vector<uint8_t> bytes;

  static Result make_ok_string(std::string s)
  {
    Result r;
    r.kind = ResultKind::OkString;
    r.str = std::move(s);
    return r;
  }

  static Result make_ok_bytes(std::vector<uint8_t> b)
  {
    Result r;
    r.kind = ResultKind::OkBytes;
    r.bytes = std::move(b);
    return r;
  }

  static Result make_err(std::string msg)
  {
    Result r;
    r.kind = ResultKind::Err;
    r.str = std::move(msg);
    return r;
  }

  static Result make_cancelled()
  {
    Result r;
    r.kind = ResultKind::Cancelled;
    return r;
  }

  bool is_ok() const
  {
    return kind == ResultKind::OkString || kind == ResultKind::OkBytes;
  }

};

struct Job {
  uint64_t task_id = 0;
  std::string fn_name;
  Args args;
  co::oneshot::Sender<Result> reply;
  std::shared_ptr<std::atomic_bool> cancelled;
};

}  // namespace job
