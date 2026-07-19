#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include "curl_http.hpp"
#include "fetch.hpp"
#include "host.hpp"

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::debug);
  spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

  const char* script = "demo.js";
  if (argc >= 2) {
    script = argv[1];
  }

  Host host;
  if (!host) {
    spdlog::error("init failed");
    return 1;
  }
  if (!host.install_runtime()) {
    spdlog::error("install_runtime failed");
    return 1;
  }
  if (!fetch_api::install(host)) {
    spdlog::error("fetch install failed");
    return 1;
  }

  spdlog::info("curl {}", curl_version());
  spdlog::info("loading {}", script);
  if (!host.eval_file(script)) {
    host.shutdown();
    return 1;
  }

  spdlog::info("running loop (pending_ops={})", host.pending_ops);
  host.run_loop();
  host.shutdown();

  spdlog::info("done");
  return 0;
}
