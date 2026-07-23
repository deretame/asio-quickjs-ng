#include "http_server.hpp"
#include <spdlog/spdlog.h>
#include <llhttp.h>
#include <cstring>
#include <algorithm>

struct Connection {
  HttpServer* server;
  llhttp_t parser;
  llhttp_settings_t settings;
  std::string buffer;
  std::string method, path, version;
  std::unordered_map<std::string, std::string> headers;
  bool headers_complete = false;
  bool message_complete = false;

  Connection(HttpServer* s) : server(s) {
    llhttp_settings_init(&settings);
    settings.on_message_begin = [](llhttp_t* p) { return HPE_OK; };
    settings.on_url = [](llhttp_t* p, const char* at, size_t len) {
      Connection* conn = static_cast<Connection*>(p->data);
      conn->path.append(at, len);
      return HPE_OK;
    };
    settings.on_header_field = [](llhttp_t* p, const char* at, size_t len) {
      Connection* conn = static_cast<Connection*>(p->data);
      conn->method = std::string(at, len);
      return HPE_OK;
    };
    settings.on_header_value = [](llhttp_t* p, const char* at, size_t len) {
      Connection* conn = static_cast<Connection*>(p->data);
      if (!conn->method.empty()) {
        conn->headers[conn->method] = std::string(at, len);
        conn->method.clear();
      }
      return HPE_OK;
    };
    settings.on_headers_complete = [](llhttp_t* p) {
      Connection* conn = static_cast<Connection*>(p->data);
      conn->headers_complete = true;
      return HPE_OK;
    };
    settings.on_body = [](llhttp_t* p, const char* at, size_t len) {
      Connection* conn = static_cast<Connection*>(p->data);
      // body handling simple - append to buffer if needed
      return HPE_OK;
    };
    settings.on_message_complete = [](llhttp_t* p) {
      Connection* conn = static_cast<Connection*>(p->data);
      conn->message_complete = true;
      return HPE_OK;
    };

    llhttp_init(&parser, HTTP_REQUEST, &settings);
    parser.data = this;
  }

  void feed(const char* data, size_t len) {
    llhttp_execute(&parser, data, len);
  }
};

HttpServer::HttpServer(asio::io_context& ioc, uint16_t port, HttpHandler handler)
    : acceptor_(ioc), handler_(std::move(handler)), socket_(ioc) {}

HttpServer::~HttpServer() {
  stop();
}

void HttpServer::start() {
  do_accept();
}

void HttpServer::stop() {
  acceptor_.close();
  socket_.close();
}

void HttpServer::do_accept() {
  acceptor_.async_accept(socket_, [this](std::error_code ec) {
    if (!ec) {
      auto conn = std::make_shared<Connection>(this);
      do_read(conn);
    } else {
      spdlog::error("accept error: {}", ec.message());
    }
    do_accept();
  });
}

void HttpServer::do_read(std::shared_ptr<Connection> conn) {
  socket_.async_read_some(asio::buffer(conn->buffer), [this, conn](std::error_code ec, std::size_t bytes) {
    if (!ec) {
      conn->buffer.resize(conn->buffer.size() + bytes);
      conn->feed(conn->buffer.data(), conn->buffer.size());
      if (conn->message_complete) {
        // call handler
        HttpRequest req;
        req.method = conn->method;
        req.path = conn->path;
        req.headers = conn->headers;
        handler_(req);
        // send response
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        asio::write(socket_, asio::buffer(resp));
        conn->buffer.clear();
        conn->message_complete = false;
        conn->headers_complete = false;
        conn->method.clear();
        conn->path.clear();
        conn->headers.clear();
      }
      do_read(conn);
    } else {
      spdlog::error("read error: {}", ec.message());
    }
  });
}

