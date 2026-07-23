#pragma once

#include <asio.hpp>
#include <llhttp.h>
#include <string>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

struct HttpRequest {
  std::string method;
  std::string path;
  std::string version;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

using HttpHandler = std::function<void(const HttpRequest& req)>;

class HttpServer {
public:
  HttpServer(asio::io_context& ioc, uint16_t port, HttpHandler handler);
  ~HttpServer();

  void start();
  void stop();

private:
  asio::ip::tcp::acceptor acceptor_;
  HttpHandler handler_;
  asio::ip::tcp::socket socket_;
  llhttp_t parser_;
  llhttp_settings_t settings_;
  std::string buffer_;
  bool parsing_ = false;

  static int on_message_begin(llhttp_t* p);
  static int on_url(llhttp_t* p, const char* at, size_t length);
  static int on_header_field(llhttp_t* p, const char* at, size_t length);
  static int on_header_value(llhttp_t* p, const char* at, size_t length);
  static int on_headers_complete(llhttp_t* p);
  static int on_body(llhttp_t* p, const char* at, size_t length);
  static int on_message_complete(llhttp_t* p);

  void do_accept();
  void do_read();
  void parse_http_data();
};

