#pragma once

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <asio.hpp>
#include <chrono>
#include <memory>
#include <unordered_map>

#include "asio_executor.hpp"
#include "curl_http.hpp"

namespace curl_http {

struct CurlWatch;
struct Transfer;
class Client;

// Forward declarations for client-internal C callbacks (defined as friends).
void on_socket_action(Client* client, curl_socket_t fd, int ev_bitmask);
int curl_socket_callback(
  CURL* /*easy*/,
  curl_socket_t s,
  int what,
  void* userp,
  void* socketp
  );
int curl_timer_callback(CURLM* /*multi*/, long timeout_ms, void* userp);

class Client : public std::enable_shared_from_this<Client> {
 public:
  explicit Client(AsioExecutor& ex);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Add the transfer's easy handle to the multi handle and start driving it.
  // The transfer's client pointer must already be set to this Client.
  bool add_transfer(Transfer* tr);

  // Remove the transfer's easy handle from the multi handle (used by
  // Transfer::finish).
  void remove_transfer(Transfer* tr);

  // Drive the multi loop. Call this after adding a new transfer.
  void notify();

  // Stop all watches and cancel the timer. Should be called before destruction.
  void shutdown();

  explicit operator bool() const {
    return static_cast<bool>(multi_);
  }

  AsioExecutor& executor() const { return ex_; }

  asio::io_context& io_context() const { return ioc_; }

 private:
  AsioExecutor& ex_;
  asio::io_context& ioc_;
  Multi multi_;
  asio::steady_timer timer_;
  std::unordered_map<curl_socket_t, std::shared_ptr<CurlWatch>> watches_;
  bool stopping_ = false;

  void on_socket_action(curl_socket_t fd, int ev_bitmask);
  void on_multi_messages();
  void arm_timer(long timeout_ms);

  friend int curl_socket_callback(
    CURL* /*easy*/,
    curl_socket_t s,
    int what,
    void* userp,
    void* socketp
    );
  friend int curl_timer_callback(
    CURLM* /*multi*/,
    long timeout_ms,
    void* userp
    );
  friend struct CurlWatch;
  friend struct Transfer;
};

}  // namespace curl_http
