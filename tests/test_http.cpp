#include <gtest/gtest.h>
#include <curl/curl.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>

#include "host.hpp"
#include "fetch.hpp"

// Helper: libcurl write callback
static size_t WriteCallback(
  void* contents, size_t size, size_t nmemb, std::string* userp)
{
  size_t total = size * nmemb;
  userp->append(static_cast<char*>(contents), total);
  return total;
}

// Helper: libcurl header callback
static size_t HeaderCallback(
  char* buffer, size_t size, size_t nitems, std::vector<std::string>* headers)
{
  size_t total = size * nitems;
  std::string header(buffer, total);
  // Remove trailing \r\n
  while (!header.empty() && (header.back() == '\r' || header.back() == '\n')) {
    header.pop_back();
  }
  if (!header.empty()) {
    headers->push_back(header);
  }
  return total;
}

// Helper: make an HTTP request
struct TestHttpResponse {
  long status = 0;
  std::string body;
  std::vector<std::string> headers;
  CURLcode code = CURLE_OK;
};

static TestHttpResponse HttpGet(const std::string& url)
{
  TestHttpResponse resp;
  CURL* curl = curl_easy_init();
  if (!curl) return resp;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

  resp.code = curl_easy_perform(curl);
  if (resp.code == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
  }
  curl_easy_cleanup(curl);
  return resp;
}

static TestHttpResponse HttpPost(
  const std::string& url, const std::string& body,
  const std::string& content_type = "application/json")
{
  TestHttpResponse resp;
  CURL* curl = curl_easy_init();
  if (!curl) return resp;

  struct curl_slist* headers = nullptr;
  std::string ct_header = "Content-Type: " + content_type;
  headers = curl_slist_append(headers, ct_header.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

  resp.code = curl_easy_perform(curl);
  if (resp.code == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return resp;
}

// Test fixture: starts a Host with HTTP server
class HttpServerTest : public ::testing::Test {
protected:
  static inline Host* host = nullptr;
  static inline std::thread server_thread;
  static inline std::atomic<bool> ready{false};
  static inline uint16_t port = 0;

  static void SetUpTestSuite() {
    // Start server in a separate thread
    server_thread = std::thread([]() {
    host = new Host();
    host->install_runtime();
    fetch_api::install(*host);

      // Create a simple JS server script
      const char* js = R"JS(
        import { Hono } from 'hono';
        const app = new Hono();

        app.get('/', (c) => c.text('Hello Hono!'));
        app.get('/ping', (c) => c.text('pong'));

        const api = new Hono();
        api.get('/users', (c) => c.json({ users: ['john', 'jane'] }));
        api.get('/users/:id', (c) => c.json({ id: c.req.param('id') }));
        app.route('/api', api);

        app.post('/echo', async (c) => {
          const body = await c.req.text();
          return c.json({ received: body.length, ok: true });
        });

        app.get('/headers', (c) => {
          return c.newResponse(JSON.stringify({
            host: c.req.header('host'),
            'user-agent': c.req.header('user-agent')
          }), 200, {
            'Content-Type': 'application/json',
            'X-Custom': 'hello'
          });
        });

        app.get('/status', (c) => c.text('Created', 201));
        app.get('/empty', (c) => c.body(null, 204));

        app.get('/stream', (c) => {
          return globalThis.stream(async (write, end) => {
            for (let i = 0; i < 5; i++) {
              await write('chunk ' + i + '\n');
            }
            await end();
          }, { contentType: 'text/plain' });
        });

        app.get('/error', () => { throw new Error('test error'); });
        app.onError((err, c) => c.json({ error: err.message }, 500));

        const handler = app.fetch.bind(app);
        const server = globalThis.createServer(handler);
        server.listen(18080);
      )JS";

      host->eval_source(js, "<test-server>", false, JS_EVAL_TYPE_MODULE);

      port = 18080;
      ready = true;

      // Run the event loop (blocks until http_active becomes false)
      host->run_loop();
    });

    // Wait for server to start
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  static void TearDownTestSuite() {
    if (host) {
      host->shutdown();
      delete host;
      host = nullptr;
    }
    if (server_thread.joinable()) {
      server_thread.join();
    }
  }
};

TEST_F(HttpServerTest, ServerStarted) {
  EXPECT_TRUE(ready.load());
  EXPECT_NE(port, 0);
}

TEST_F(HttpServerTest, BasicGet) {
  auto resp = HttpGet("http://localhost:18080/");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 200);
  EXPECT_EQ(resp.body, "Hello Hono!");
}

TEST_F(HttpServerTest, Ping) {
  auto resp = HttpGet("http://localhost:18080/ping");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 200);
  EXPECT_EQ(resp.body, "pong");
}

TEST_F(HttpServerTest, NestedRoute) {
  auto resp = HttpGet("http://localhost:18080/api/users");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 200);
  EXPECT_EQ(resp.body, "{\"users\":[\"john\",\"jane\"]}");
}

TEST_F(HttpServerTest, RouteParam) {
  auto resp = HttpGet("http://localhost:18080/api/users/42");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 200);
  EXPECT_EQ(resp.body, "{\"id\":\"42\"}");
}

TEST_F(HttpServerTest, PostEcho) {
  auto resp = HttpPost("http://localhost:18080/echo", "hello world");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 200);
  EXPECT_NE(resp.body.find("\"received\":11"), std::string::npos);
  EXPECT_NE(resp.body.find("\"ok\":true"), std::string::npos);
}

TEST_F(HttpServerTest, CustomHeaders) {
  auto resp = HttpGet("http://localhost:18080/headers");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 200);
  // Check custom header is present (HTTP headers are case-insensitive)
  bool has_custom = false;
  for (const auto& h : resp.headers) {
    if (h.find("x-custom: hello") != std::string::npos ||
        h.find("X-Custom: hello") != std::string::npos) {
      has_custom = true;
    }
  }
  EXPECT_TRUE(has_custom);
}

TEST_F(HttpServerTest, CustomStatus) {
  auto resp = HttpGet("http://localhost:18080/status");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 201);
  EXPECT_EQ(resp.body, "Created");
}

TEST_F(HttpServerTest, EmptyBody) {
  auto resp = HttpGet("http://localhost:18080/empty");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 204);
  EXPECT_TRUE(resp.body.empty());
}

TEST_F(HttpServerTest, Streaming) {
  auto resp = HttpGet("http://localhost:18080/stream");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 200);
  EXPECT_EQ(resp.body, "chunk 0\nchunk 1\nchunk 2\nchunk 3\nchunk 4\n");
}

TEST_F(HttpServerTest, ErrorHandling) {
  auto resp = HttpGet("http://localhost:18080/error");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 500);
  EXPECT_NE(resp.body.find("test error"), std::string::npos);
}

TEST_F(HttpServerTest, NotFound) {
  auto resp = HttpGet("http://localhost:18080/nonexistent");
  EXPECT_EQ(resp.code, CURLE_OK);
  EXPECT_EQ(resp.status, 404);
}
