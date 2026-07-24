#pragma once

#include <asio.hpp>
#include <llhttp.h>
#include <span>
#include <string>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdint>

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::string version;
  std::unordered_map<std::string, std::string> headers;
  std::vector<uint8_t> body;
  std::string url;
};

struct HttpResponse {
  int status = 200;
  std::string status_text;
  std::unordered_map<std::string, std::string> headers;
  std::vector<uint8_t> body;
};

class HttpSession;

class HttpServer {
public:
  using RequestHandler = std::function<void(
    std::shared_ptr<HttpSession> session,
    HttpRequest req
  )>;

  HttpServer(asio::io_context& ioc, uint16_t port, RequestHandler handler);
  ~HttpServer();

  void start();
  void stop();

private:
  asio::io_context& ioc_;
  asio::ip::tcp::acceptor acceptor_;
  RequestHandler handler_;
  uint16_t port_;

  void do_accept();
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(asio::ip::tcp::socket socket, HttpServer::RequestHandler handler);

  void start();

  asio::ip::tcp::socket& socket() { return socket_; }

  void send_response(const HttpResponse& resp);
  void send_raw(std::string_view data);

  // Begin a chunked transfer encoding response.
  // Caller must call finish_chunked() when done.
  void begin_chunked(int status, std::string_view content_type);

  // Send one chunk of body data in chunked transfer encoding.
  // The callback is called when the write completes.
  void write_chunk(
    std::span<const uint8_t> data,
    std::function<void(std::error_code)> cb = nullptr);

  // Synchronous version - blocks until written (OK for single-threaded).
  void write_chunk_sync(std::span<const uint8_t> data);

  // Finish the chunked response (send final 0-length chunk).
  // If keep_alive is true, the connection stays open for reuse.
  void finish_chunked(
    bool keep_alive = true,
    std::function<void(std::error_code)> cb = nullptr);

 private:
  asio::ip::tcp::socket socket_;
  HttpServer::RequestHandler handler_;

  llhttp_t parser_;
  llhttp_settings_t settings_;

  std::vector<uint8_t> buffer_;
  HttpRequest current_req_;
  std::string current_header_name_;
  std::string current_header_value_;
  bool headers_complete_ = false;
  bool message_complete_ = false;
  bool close_after_response_ = true;

  static int on_message_begin(llhttp_t* p);
  static int on_url(llhttp_t* p, const char* at, size_t length);
  static int on_header_field(llhttp_t* p, const char* at, size_t length);
  static int on_header_value(llhttp_t* p, const char* at, size_t length);
  static int on_headers_complete(llhttp_t* p);
  static int on_body(llhttp_t* p, const char* at, size_t length);
  static int on_message_complete(llhttp_t* p);

  void do_read();
  void parse_http_data(size_t bytes);
  void handle_request();
};
