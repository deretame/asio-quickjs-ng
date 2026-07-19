#pragma once

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Minimal loopback HTTP/1.1 server for fetch tests (separate thread/ioc).
// Acts as a stand-in for wptserve resources used by fetch WPT.
class HttpTestServer {
public:
  HttpTestServer() : acceptor_(ioc_) {}

  ~HttpTestServer() { stop(); }

  void start() {
    asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
    running_ = true;
    do_accept();
    thr_ = std::thread([this] { ioc_.run(); });
  }

  void stop() {
    if (!running_.exchange(false)) {
      return;
    }
    asio::post(ioc_, [this] {
      asio::error_code ec;
      acceptor_.close(ec);
      ioc_.stop();
    });
    if (thr_.joinable()) {
      thr_.join();
    }
    ioc_.restart();
  }

  uint16_t port() const { return port_; }

  std::string origin() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

private:
  struct Conn : std::enable_shared_from_this<Conn> {
    explicit Conn(asio::io_context &ioc) : sock(ioc) {}
    asio::ip::tcp::socket sock;
    asio::streambuf buf;
  };

  void do_accept() {
    auto c = std::make_shared<Conn>(ioc_);
    acceptor_.async_accept(c->sock, [this, c](const asio::error_code &ec) {
      if (!ec && running_) {
        do_read(c);
        do_accept();
      }
    });
  }

  void do_read(std::shared_ptr<Conn> c) {
    asio::async_read_until(
        c->sock, c->buf, "\r\n\r\n",
        [this, c](const asio::error_code &ec, std::size_t n) {
          if (ec) {
            return;
          }
          std::string head(asio::buffers_begin(c->buf.data()),
                           asio::buffers_begin(c->buf.data()) +
                               static_cast<std::ptrdiff_t>(n));
          c->buf.consume(n);

          std::string method;
          std::string target;
          std::string version;
          std::istringstream hs(head);
          hs >> method >> target >> version;

          std::map<std::string, std::string> headers;
          std::string line;
          std::getline(hs, line);
          while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }
            if (line.empty()) {
              break;
            }
            auto pos = line.find(':');
            if (pos == std::string::npos) {
              continue;
            }
            std::string name = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
              value.erase(value.begin());
            }
            for (char &ch : name) {
              if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
              }
            }
            headers[name] = value;
          }

          std::size_t content_length = 0;
          if (auto it = headers.find("content-length"); it != headers.end()) {
            content_length = static_cast<std::size_t>(std::stoul(it->second));
          }

          auto finish = [this, c, method, target,
                         headers](std::string body) mutable {
            int delay_ms = 0;
            std::string path = target;
            auto qpos = path.find('?');
            std::string query;
            if (qpos != std::string::npos) {
              query = path.substr(qpos + 1);
              path = path.substr(0, qpos);
            }
            if (path == "/slow" || path == "/infinite-slow-response") {
              delay_ms = 500;
              if (auto p = query.find("ms="); p != std::string::npos) {
                delay_ms = std::atoi(query.c_str() + p + 3);
              }
            }

            auto write_resp = [c, method, target, headers,
                               body = std::move(body)]() mutable {
              auto resp = build_response(method, target, headers, body);
              auto msg = std::make_shared<std::string>(std::move(resp));
              asio::async_write(
                  c->sock, asio::buffer(*msg),
                  [c, msg](const asio::error_code &, std::size_t) {
                    asio::error_code ec2;
                    c->sock.shutdown(asio::ip::tcp::socket::shutdown_both, ec2);
                    c->sock.close(ec2);
                  });
            };

            if (delay_ms <= 0) {
              write_resp();
              return;
            }
            auto timer = std::make_shared<asio::steady_timer>(
                c->sock.get_executor(), std::chrono::milliseconds(delay_ms));
            timer->async_wait([timer, write_resp = std::move(write_resp)](
                                  const asio::error_code &err) mutable {
              if (!err) {
                write_resp();
              }
            });
          };

          if (content_length == 0) {
            finish({});
            return;
          }

          if (c->buf.size() >= content_length) {
            std::string body(asio::buffers_begin(c->buf.data()),
                             asio::buffers_begin(c->buf.data()) +
                                 static_cast<std::ptrdiff_t>(content_length));
            c->buf.consume(content_length);
            finish(std::move(body));
            return;
          }

          const std::size_t need = content_length - c->buf.size();
          asio::async_read(
              c->sock, c->buf, asio::transfer_exactly(need),
              [c, content_length, finish](const asio::error_code &ec2,
                                          std::size_t) mutable {
                if (ec2) {
                  return;
                }
                std::string body(
                    asio::buffers_begin(c->buf.data()),
                    asio::buffers_begin(c->buf.data()) +
                        static_cast<std::ptrdiff_t>(content_length));
                c->buf.consume(content_length);
                finish(std::move(body));
              });
        });
  }

  static std::string build_response(
      const std::string &method, const std::string &target,
      const std::map<std::string, std::string> &headers,
      const std::string &req_body) {
    std::string path = target;
    std::string query;
    auto q = path.find('?');
    if (q != std::string::npos) {
      query = path.substr(q + 1);
      path = path.substr(0, q);
    }

    int status = 200;
    std::string status_text = "OK";
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extra;

    // WPT-like aliases
    if (path == "/resources/data.json" || path == "/data.json") {
      content_type = "application/json";
      body = "{\"key\":\"value\"}";
    } else if (path == "/" || path == "/text") {
      body = "Hello WPT";
    } else if (path == "/json") {
      content_type = "application/json";
      body = "{\"hello\":\"world\"}";
    } else if (path == "/echo") {
      body = req_body;
      if (auto it = headers.find("content-type"); it != headers.end()) {
        content_type = it->second;
      }
    } else if (path.rfind("/status/", 0) == 0) {
      status = std::atoi(path.c_str() + 8);
      if (status <= 0) {
        status = 404;
      }
      status_text = (status == 404)   ? "Not Found"
                    : (status == 500) ? "Internal Server Error"
                                      : "Status";
      body = "status-body";
    } else if (path == "/redirect" || path == "/redirect/text") {
      status = 302;
      status_text = "Found";
      extra.emplace_back("Location", "/text");
      body = "";
    } else if (path == "/redirect/json") {
      status = 302;
      status_text = "Found";
      extra.emplace_back("Location", "/json");
      body = "";
    } else if (path == "/headers") {
      content_type = "application/json";
      std::ostringstream ss;
      ss << "{\"method\":\"" << method << "\"";
      if (auto it = headers.find("x-test"); it != headers.end()) {
        ss << ",\"x-test\":\"" << it->second << "\"";
      }
      if (auto it = headers.find("content-type"); it != headers.end()) {
        ss << ",\"content-type\":\"" << it->second << "\"";
      }
      if (auto it = headers.find("accept"); it != headers.end()) {
        ss << ",\"accept\":\"" << it->second << "\"";
      }
      ss << "}";
      body = ss.str();
    } else if (path == "/slow" || path == "/infinite-slow-response") {
      body = "slow-ok";
    } else if (path == "/method") {
      body = method;
    } else if (path == "/empty") {
      body = "";
    } else {
      status = 404;
      status_text = "Not Found";
      body = "no route: " + path;
    }

    if (method == "HEAD") {
      body.clear();
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << status << " " << status_text << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: close\r\n";
    for (const auto &h : extra) {
      out << h.first << ": " << h.second << "\r\n";
    }
    out << "X-Test-Server: asio-qjs\r\n";
    out << "\r\n";
    out << body;
    return out.str();
  }

  asio::io_context ioc_{1};
  asio::ip::tcp::acceptor acceptor_;
  std::thread thr_;
  std::atomic<bool> running_{false};
  uint16_t port_ = 0;
};
