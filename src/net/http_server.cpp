#include "http_server.hpp"
#include <spdlog/spdlog.h>
#include <llhttp.h>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

HttpServer::HttpServer(
  asio::io_context& ioc,
  uint16_t port,
  RequestHandler handler
)
  : ioc_(ioc)
  , acceptor_(ioc)
  , handler_(std::move(handler))
  , port_(port)
{
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
  asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port_);
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen();
  spdlog::info("HTTP server listening on port {}", port_);
  do_accept();
}

void HttpServer::stop() {
  if (acceptor_.is_open()) {
    acceptor_.close();
  }
}

void HttpServer::do_accept() {
  acceptor_.async_accept(
    [this](std::error_code ec, asio::ip::tcp::socket socket) {
      if (!ec) {
        auto session = std::make_shared<HttpSession>(
          std::move(socket), handler_);
        session->start();
      } else {
        if (ec != asio::error::operation_aborted) {
          spdlog::debug("accept stopped: {}", ec.message());
        }
      }
      // Only continue accepting if the acceptor is still open
      if (acceptor_.is_open()) {
        do_accept();
      }
    });
}

// ---------------------------------------------------------------------------
// HttpSession
// ---------------------------------------------------------------------------

HttpSession::HttpSession(
  asio::ip::tcp::socket socket,
  HttpServer::RequestHandler handler
)
  : socket_(std::move(socket))
  , handler_(std::move(handler))
{
  llhttp_settings_init(&settings_);
  settings_.on_message_begin = &HttpSession::on_message_begin;
  settings_.on_url = &HttpSession::on_url;
  settings_.on_header_field = &HttpSession::on_header_field;
  settings_.on_header_value = &HttpSession::on_header_value;
  settings_.on_headers_complete = &HttpSession::on_headers_complete;
  settings_.on_body = &HttpSession::on_body;
  settings_.on_message_complete = &HttpSession::on_message_complete;

  llhttp_init(&parser_, HTTP_REQUEST, &settings_);
  parser_.data = this;
}

void HttpSession::start() {
  do_read();
}

void HttpSession::do_read() {
  auto self = shared_from_this();
  buffer_.resize(buffer_.size() + 4096);
  socket_.async_read_some(
    asio::buffer(buffer_.data() + buffer_.size() - 4096, 4096),
    [this, self](std::error_code ec, std::size_t bytes) {
      if (ec) {
        if (ec != asio::error::operation_aborted &&
            ec != asio::error::eof &&
            ec != asio::error::connection_reset) {
          spdlog::debug("read error: {}", ec.message());
        }
        return;
      }
      buffer_.resize(buffer_.size() - 4096 + bytes);
      parse_http_data(bytes);
      if (message_complete_) {
        handle_request();
      } else {
        do_read();
      }
    });
}

void HttpSession::parse_http_data(size_t bytes) {
  if (buffer_.empty()) return;
  enum llhttp_errno err = llhttp_execute(
    &parser_,
    reinterpret_cast<const char*>(buffer_.data()),
    buffer_.size());
  if (err != HPE_OK) {
    spdlog::debug("parse error: {}", llhttp_errno_name(err));
    close_after_response_ = true;
  }
}

void HttpSession::handle_request() {
  handler_(shared_from_this(), std::move(current_req_));
  // Reset for potential keep-alive
  buffer_.clear();
  current_req_ = {};
  current_header_name_.clear();
  current_header_value_.clear();
  headers_complete_ = false;
  message_complete_ = false;
  llhttp_reset(&parser_);
}

void HttpSession::send_response(const HttpResponse& resp) {
  std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " +
    (resp.status_text.empty() ? "OK" : resp.status_text) + "\r\n";

  for (const auto& [k, v] : resp.headers) {
    out += k + ": " + v + "\r\n";
  }

  auto it = resp.headers.find("content-length");
  if (it == resp.headers.end()) {
    out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
  }

  if (close_after_response_) {
    out += "Connection: close\r\n";
  } else {
    out += "Connection: keep-alive\r\n";
  }

  out += "\r\n";

  // Prepend headers to body
  std::vector<uint8_t> full;
  full.reserve(out.size() + resp.body.size());
  full.insert(full.end(), out.begin(), out.end());
  full.insert(full.end(), resp.body.begin(), resp.body.end());

  auto self = shared_from_this();
  asio::async_write(
    socket_,
    asio::buffer(full),
    [this, self](std::error_code ec, std::size_t) {
      if (ec) {
        spdlog::debug("write error: {}", ec.message());
        return;
      }
      if (close_after_response_) {
        // Close connection
        std::error_code ignore;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
        socket_.close(ignore);
      } else {
        // Keep-alive: read next request
        do_read();
      }
    });
}

void HttpSession::send_raw(std::string_view data) {
  auto self = shared_from_this();
  auto buf = std::make_shared<std::string>(data);
  asio::async_write(
    socket_,
    asio::buffer(*buf),
    [self, buf](std::error_code, std::size_t) {});
}

void HttpSession::begin_chunked(int status, std::string_view content_type) {
  std::string out = "HTTP/1.1 " + std::to_string(status) + " OK\r\n";
  out += "Content-Type: ";
  out += content_type;
  out += "\r\n";
  out += "Transfer-Encoding: chunked\r\n";
  out += "Connection: keep-alive\r\n";
  out += "\r\n";
  send_raw(out);
}

void HttpSession::write_chunk(
  std::span<const uint8_t> data,
  std::function<void(std::error_code)> cb)
{
  if (data.empty()) {
    if (cb) cb(std::error_code());
    return;
  }
  // Chunk format: <hex-size>\r\n<data>\r\n
  char hex[32];
  snprintf(hex, sizeof(hex), "%zx\r\n", data.size());
  std::string header(hex);
  std::string footer = "\r\n";

  // Build full write buffer
  std::vector<uint8_t> buf;
  buf.reserve(header.size() + data.size() + footer.size());
  buf.insert(buf.end(), header.begin(), header.end());
  buf.insert(buf.end(), data.begin(), data.end());
  buf.insert(buf.end(), footer.begin(), footer.end());

  spdlog::debug("write_chunk: {} bytes", data.size());

  auto self = shared_from_this();
  auto shared_buf = std::make_shared<std::vector<uint8_t>>(std::move(buf));
  spdlog::debug("async_write queued: {} bytes", shared_buf->size());
  asio::async_write(
    socket_,
    asio::buffer(*shared_buf),
    [self, shared_buf, cb](std::error_code ec, std::size_t bytes) {
      if (ec) {
        spdlog::debug("chunk write error: {}", ec.message());
      } else {
        spdlog::debug("chunk write done: {} bytes", bytes);
      }
      if (cb) cb(ec);
    });
}

void HttpSession::write_chunk_sync(std::span<const uint8_t> data) {
  if (data.empty()) return;
  char hex[32];
  snprintf(hex, sizeof(hex), "%zx\r\n", data.size());
  std::string header(hex);
  std::string footer = "\r\n";

  std::vector<uint8_t> buf;
  buf.reserve(header.size() + data.size() + footer.size());
  buf.insert(buf.end(), header.begin(), header.end());
  buf.insert(buf.end(), data.begin(), data.end());
  buf.insert(buf.end(), footer.begin(), footer.end());

  spdlog::debug("write_chunk_sync: {} bytes", data.size());
  asio::error_code ec;
  asio::write(socket_, asio::buffer(buf), ec);
  if (ec) {
    spdlog::debug("write_chunk_sync error: {}", ec.message());
  }
}

void HttpSession::finish_chunked(
  bool keep_alive,
  std::function<void(std::error_code)> cb)
{
  // Final chunk: 0\r\n\r\n
  std::string final_chunk = "0\r\n\r\n";
  spdlog::debug("finish_chunked: keep_alive={}", keep_alive);
  auto self = shared_from_this();
  auto buf = std::make_shared<std::string>(final_chunk);
  asio::async_write(
    socket_,
    asio::buffer(*buf),
    [this, self, buf, keep_alive, cb](std::error_code ec, std::size_t) {
      if (ec) {
        spdlog::debug("final chunk write error: {}", ec.message());
        if (cb) cb(ec);
        return;
      }
      spdlog::debug("final chunk sent");
      if (keep_alive) {
        do_read();
      } else {
        std::error_code ignore;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
        socket_.close(ignore);
      }
      if (cb) cb(std::error_code());
    });
}

// ---------------------------------------------------------------------------
// llhttp callbacks
// ---------------------------------------------------------------------------

int HttpSession::on_message_begin(llhttp_t* p) {
  auto* self = static_cast<HttpSession*>(p->data);
  self->current_req_ = {};
  self->current_header_name_.clear();
  self->current_header_value_.clear();
  self->headers_complete_ = false;
  self->message_complete_ = false;
  return HPE_OK;
}

int HttpSession::on_url(llhttp_t* p, const char* at, size_t length) {
  auto* self = static_cast<HttpSession*>(p->data);
  self->current_req_.url.append(at, length);

  std::string url(at, length);
  auto qpos = url.find('?');
  if (qpos != std::string::npos) {
    self->current_req_.path = url.substr(0, qpos);
    self->current_req_.query = url.substr(qpos + 1);
  } else {
    self->current_req_.path = url;
    self->current_req_.query.clear();
  }

  self->current_req_.method =
    llhttp_method_name(static_cast<llhttp_method_t>(p->method));
  return HPE_OK;
}

int HttpSession::on_header_field(llhttp_t* p, const char* at, size_t length) {
  auto* self = static_cast<HttpSession*>(p->data);
  if (!self->current_header_value_.empty() &&
      !self->current_header_name_.empty()) {
    // Store previous header
    std::string lower = self->current_header_name_;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    self->current_req_.headers[lower] = self->current_header_value_;
    self->current_header_value_.clear();
  }
  self->current_header_name_.append(at, length);
  return HPE_OK;
}

int HttpSession::on_header_value(llhttp_t* p, const char* at, size_t length) {
  auto* self = static_cast<HttpSession*>(p->data);
  self->current_header_value_.append(at, length);
  return HPE_OK;
}

int HttpSession::on_headers_complete(llhttp_t* p) {
  auto* self = static_cast<HttpSession*>(p->data);
  // Store last header
  if (!self->current_header_name_.empty()) {
    std::string lower = self->current_header_name_;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    self->current_req_.headers[lower] = self->current_header_value_;
    self->current_header_name_.clear();
    self->current_header_value_.clear();
  }
  self->headers_complete_ = true;

  // Check for keep-alive
  auto conn = self->current_req_.headers.find("connection");
  if (conn != self->current_req_.headers.end()) {
    std::string val = conn->second;
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    self->close_after_response_ = (val == "close");
  } else {
    self->close_after_response_ = (p->http_major == 1 && p->http_minor == 0);
  }

  return HPE_OK;
}

int HttpSession::on_body(llhttp_t* p, const char* at, size_t length) {
  auto* self = static_cast<HttpSession*>(p->data);
  self->current_req_.body.insert(
    self->current_req_.body.end(),
    reinterpret_cast<const uint8_t*>(at),
    reinterpret_cast<const uint8_t*>(at) + length);
  return HPE_OK;
}

int HttpSession::on_message_complete(llhttp_t* p) {
  auto* self = static_cast<HttpSession*>(p->data);
  self->message_complete_ = true;
  return HPE_OK;
}
