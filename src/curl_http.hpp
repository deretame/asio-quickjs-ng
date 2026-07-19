#pragma once

#include <asio.hpp>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>

namespace curl_http {

class Runtime;

struct HeaderPair {
  std::string name;
  std::string value;
};

struct FetchOptions {
  std::string url;
  std::string method = "GET";
  std::vector<HeaderPair> headers;
  std::string body;
  bool follow_redirects = true;
  long timeout_sec = 30;
  // When true, a 3xx final status is treated as network failure (redirect: "error").
  bool fail_on_redirect = false;
};

struct FetchResult {
  // true when the transfer completed at the HTTP layer (any status).
  bool ok = false;
  long status = 0;
  std::string status_text;
  std::string body;
  std::string url;
  std::string error;
  std::vector<HeaderPair> headers;
  bool redirected = false;
  bool aborted = false;
};

class Global {
public:
  Global() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~Global() { curl_global_cleanup(); }
  Global(const Global &) = delete;
  Global &operator=(const Global &) = delete;
};

class Easy {
public:
  Easy() : easy_(curl_easy_init()) {}
  ~Easy() { reset(); }

  Easy(const Easy &) = delete;
  Easy &operator=(const Easy &) = delete;

  Easy(Easy &&o) noexcept : easy_(o.easy_) { o.easy_ = nullptr; }
  Easy &operator=(Easy &&o) noexcept {
    if (this != &o) {
      reset();
      easy_ = o.easy_;
      o.easy_ = nullptr;
    }
    return *this;
  }

  explicit operator bool() const { return easy_ != nullptr; }
  CURL *get() const { return easy_; }

  void reset() {
    if (easy_) {
      curl_easy_cleanup(easy_);
      easy_ = nullptr;
    }
  }

  template <typename T>
  void setopt(CURLoption opt, T value) {
    curl_easy_setopt(easy_, opt, value);
  }

  template <typename T>
  bool getinfo(CURLINFO info, T *out) const {
    return curl_easy_getinfo(easy_, info, out) == CURLE_OK;
  }

private:
  CURL *easy_ = nullptr;
};

class Multi {
public:
  Multi() : multi_(curl_multi_init()) {}
  ~Multi() {
    if (multi_) {
      curl_multi_cleanup(multi_);
    }
  }

  Multi(const Multi &) = delete;
  Multi &operator=(const Multi &) = delete;

  explicit operator bool() const { return multi_ != nullptr; }
  CURLM *get() const { return multi_; }

  void set_socket_function(curl_socket_callback cb, void *data) {
    curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, cb);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, data);
  }

  void set_timer_function(curl_multi_timer_callback cb, void *data) {
    curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, cb);
    curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, data);
  }

  CURLMcode add(CURL *easy) { return curl_multi_add_handle(multi_, easy); }

  void remove(CURL *easy) {
    if (multi_ && easy) {
      curl_multi_remove_handle(multi_, easy);
    }
  }

  void assign(curl_socket_t s, void *socketp) {
    curl_multi_assign(multi_, s, socketp);
  }

  CURLMcode socket_action(curl_socket_t fd, int ev_bitmask, int *running) {
    return curl_multi_socket_action(multi_, fd, ev_bitmask, running);
  }

  CURLMsg *info_read(int *msgs_left) {
    return curl_multi_info_read(multi_, msgs_left);
  }

private:
  CURLM *multi_ = nullptr;
};

struct Transfer {
  Runtime *runtime = nullptr;
  Easy easy;
  FetchOptions options;
  std::string body;
  std::vector<HeaderPair> response_headers;
  std::string status_text;
  char errbuf[CURL_ERROR_SIZE]{};
  curl_slist *req_headers = nullptr;
  std::function<void(FetchResult)> complete;
  uint64_t id = 0;
  bool finished = false;

  ~Transfer() {
    if (req_headers) {
      curl_slist_free_all(req_headers);
      req_headers = nullptr;
    }
  }

  static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
  static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata);

  void finish(FetchResult result);
  void abort();
  bool start();
  FetchResult make_result(CURLcode code);
};

inline bool assign_socket(asio::ip::tcp::socket &sock, curl_socket_t s) {
  asio::error_code ec;
  sock.assign(asio::ip::tcp::v4(), s, ec);
  if (!ec) {
    return true;
  }
  sock.assign(asio::ip::tcp::v6(), s, ec);
  return !ec;
}

} // namespace curl_http
