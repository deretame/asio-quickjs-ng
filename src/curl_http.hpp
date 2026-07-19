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
  Multi *multi = nullptr;
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

  static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *self = static_cast<Transfer *>(userdata);
    self->body.append(ptr, size * nmemb);
    return size * nmemb;
  }

  static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *self = static_cast<Transfer *>(userdata);
    const size_t n = size * nmemb;
    std::string line(ptr, n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (line.empty()) {
      return n;
    }
    if (line.rfind("HTTP/", 0) == 0) {
      self->response_headers.clear();
      self->status_text.clear();
      // HTTP/1.1 200 OK
      auto sp1 = line.find(' ');
      if (sp1 != std::string::npos) {
        auto sp2 = line.find(' ', sp1 + 1);
        if (sp2 != std::string::npos && sp2 + 1 < line.size()) {
          self->status_text = line.substr(sp2 + 1);
        }
      }
      return n;
    }
    auto colon = line.find(':');
    if (colon == std::string::npos) {
      return n;
    }
    HeaderPair hp;
    hp.name = line.substr(0, colon);
    hp.value = line.substr(colon + 1);
    while (!hp.value.empty() && (hp.value.front() == ' ' || hp.value.front() == '\t')) {
      hp.value.erase(hp.value.begin());
    }
    self->response_headers.push_back(std::move(hp));
    return n;
  }

  void finish(FetchResult result) {
    if (finished) {
      return;
    }
    finished = true;
    auto done = std::move(complete);
    if (easy && multi) {
      easy.setopt(CURLOPT_PRIVATE, nullptr);
      multi->remove(easy.get());
      easy.reset();
    }
    if (req_headers) {
      curl_slist_free_all(req_headers);
      req_headers = nullptr;
    }
    if (done) {
      done(std::move(result));
    }
  }

  void abort() {
    FetchResult r;
    r.ok = false;
    r.aborted = true;
    r.error = "aborted";
    r.url = options.url;
    finish(std::move(r));
  }

  bool start() {
    if (!easy) {
      return false;
    }
    easy.setopt(CURLOPT_URL, options.url.c_str());
    easy.setopt(CURLOPT_FOLLOWLOCATION, options.follow_redirects ? 1L : 0L);
    easy.setopt(CURLOPT_MAXREDIRS, 20L);
    easy.setopt(CURLOPT_WRITEFUNCTION, &Transfer::write_cb);
    easy.setopt(CURLOPT_WRITEDATA, this);
    easy.setopt(CURLOPT_HEADERFUNCTION, &Transfer::header_cb);
    easy.setopt(CURLOPT_HEADERDATA, this);
    easy.setopt(CURLOPT_PRIVATE, this);
    easy.setopt(CURLOPT_ERRORBUFFER, errbuf);
    easy.setopt(CURLOPT_USERAGENT, "asio-quickjs-ng/0.1");
    easy.setopt(CURLOPT_ACCEPT_ENCODING, "");
    easy.setopt(CURLOPT_TIMEOUT, options.timeout_sec);
#ifdef CURLSSLOPT_NATIVE_CA
    easy.setopt(CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif

    const std::string method = options.method.empty() ? "GET" : options.method;
    if (method == "GET") {
      easy.setopt(CURLOPT_HTTPGET, 1L);
    } else if (method == "HEAD") {
      easy.setopt(CURLOPT_NOBODY, 1L);
    } else if (method == "POST") {
      easy.setopt(CURLOPT_POST, 1L);
      easy.setopt(CURLOPT_POSTFIELDS, options.body.c_str());
      easy.setopt(CURLOPT_POSTFIELDSIZE_LARGE,
                  static_cast<curl_off_t>(options.body.size()));
    } else if (method == "PUT") {
      easy.setopt(CURLOPT_CUSTOMREQUEST, "PUT");
      easy.setopt(CURLOPT_POSTFIELDS, options.body.c_str());
      easy.setopt(CURLOPT_POSTFIELDSIZE_LARGE,
                  static_cast<curl_off_t>(options.body.size()));
    } else {
      easy.setopt(CURLOPT_CUSTOMREQUEST, method.c_str());
      if (!options.body.empty()) {
        easy.setopt(CURLOPT_POSTFIELDS, options.body.c_str());
        easy.setopt(CURLOPT_POSTFIELDSIZE_LARGE,
                    static_cast<curl_off_t>(options.body.size()));
      }
    }

    for (const auto &h : options.headers) {
      std::string line = h.name + ": " + h.value;
      req_headers = curl_slist_append(req_headers, line.c_str());
    }
    if (req_headers) {
      easy.setopt(CURLOPT_HTTPHEADER, req_headers);
    }

    return multi->add(easy.get()) == CURLM_OK;
  }

  FetchResult make_result(CURLcode code) {
    FetchResult r;
    r.url = options.url;
    char *effective = nullptr;
    if (easy.getinfo(CURLINFO_EFFECTIVE_URL, &effective) && effective) {
      r.url = effective;
    }
    easy.getinfo(CURLINFO_RESPONSE_CODE, &r.status);
    long redirect_count = 0;
    easy.getinfo(CURLINFO_REDIRECT_COUNT, &redirect_count);
    r.redirected = redirect_count > 0;
    r.status_text = status_text;
    r.headers = std::move(response_headers);
    if (code == CURLE_OK) {
      if (options.fail_on_redirect && r.status >= 300 && r.status < 400) {
        r.ok = false;
        r.error = "redirect not allowed";
        r.body = std::move(body);
      } else {
        r.ok = true;
        r.body = std::move(body);
      }
    } else {
      r.ok = false;
      r.error = errbuf[0] ? errbuf : curl_easy_strerror(code);
    }
    return r;
  }
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
