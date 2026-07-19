cmake_minimum_required(VERSION 3.23)
project(asio_quickjs_ng LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Header-only deps have no usable root CMakeLists; Populate is intentional.
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

# --- spdlog ---
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.15.3
  GIT_SHALLOW TRUE
)

# --- asio (header-only) ---
FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-38-2
  GIT_SHALLOW TRUE
)

# --- quickjs-ng ---
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  quickjs
  GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)

# --- curl ---
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(CURL_ZLIB ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  curl
  GIT_REPOSITORY https://github.com/curl/curl.git
  GIT_TAG curl-8_14_1
  GIT_SHALLOW TRUE
)

# --- async_simple (headers only; skip broken optional submodules) ---
FetchContent_Declare(
  async_simple
  GIT_REPOSITORY https://github.com/alibaba/async_simple.git
  GIT_TAG 1.4
  GIT_SHALLOW TRUE
  GIT_SUBMODULES ""
)

# --- nlohmann/json (dynamic JSON DOM) ---
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
)

# --- reflect-cpp (struct <-> JSON via reflection; bundles yyjson) ---
set(REFLECTCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(REFLECTCPP_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(REFLECTCPP_INSTALL OFF CACHE BOOL "" FORCE)
set(REFLECTCPP_USE_VCPKG OFF CACHE BOOL "" FORCE)
set(REFLECTCPP_USE_BUNDLED_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(REFLECTCPP_JSON ON CACHE BOOL "" FORCE)
set(REFLECTCPP_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  reflectcpp
  GIT_REPOSITORY https://github.com/getml/reflect-cpp.git
  GIT_TAG v0.25.0
  GIT_SHALLOW TRUE
)

# --- stduuid (header-only UUID library) ---
FetchContent_Declare(
  stduuid
  GIT_REPOSITORY https://github.com/mariusbancila/stduuid.git
  GIT_TAG v1.2.3
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(spdlog quickjs curl nlohmann_json reflectcpp stduuid)

FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
  FetchContent_Populate(asio)
endif()
add_library(asio_header INTERFACE)
target_include_directories(asio_header INTERFACE "${asio_SOURCE_DIR}/include")
target_compile_definitions(asio_header INTERFACE
  ASIO_STANDALONE
  ASIO_NO_DEPRECATED
)

FetchContent_GetProperties(async_simple)
if(NOT async_simple_POPULATED)
  FetchContent_Populate(async_simple)
endif()
add_library(async_simple_headers INTERFACE)
target_include_directories(async_simple_headers INTERFACE "${async_simple_SOURCE_DIR}")
target_compile_definitions(async_simple_headers INTERFACE ASYNC_SIMPLE_HAS_NOT_AIO)

# --- core library (host + fetch layers) ---
add_library(asio_qjs_core
  src/curl_runtime.cpp
  src/host.cpp
  src/fetch.cpp
  src/function_registry.cpp
)
target_include_directories(asio_qjs_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(asio_qjs_core PUBLIC
  CURL_STATICLIB
  _WIN32_WINNT=0x0A00
)
target_link_libraries(asio_qjs_core PUBLIC
  asio_header
  qjs
  CURL::libcurl
  spdlog::spdlog
  async_simple_headers
  nlohmann_json::nlohmann_json
  reflectcpp::reflectcpp
  stduuid
)
if(WIN32)
  target_link_libraries(asio_qjs_core PUBLIC
    ws2_32
    mswsock
    bcrypt
    advapi32
    crypt32
  )
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(asio_qjs_core PRIVATE -Wall -Wextra -fcoroutines
    -Wno-maybe-uninitialized)
  # fetch.cpp uses #embed (C23 / Clang); silence pedantic noise
  set_source_files_properties(src/fetch.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-c23-extensions")
endif()
set_source_files_properties(src/fetch.cpp PROPERTIES
  OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/js/abort.js;${CMAKE_SOURCE_DIR}/src/js/text-encoding-polyfill.js;${CMAKE_SOURCE_DIR}/src/js/whatwg-url-polyfill.js;${CMAKE_SOURCE_DIR}/src/js/body_polyfill.js;${CMAKE_SOURCE_DIR}/src/js/headers.js;${CMAKE_SOURCE_DIR}/src/js/request.js;${CMAKE_SOURCE_DIR}/src/js/response.js;${CMAKE_SOURCE_DIR}/src/js/fetch.js"
)

add_executable(asio_qjs src/main.cpp)
target_link_libraries(asio_qjs PRIVATE asio_qjs_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(asio_qjs PRIVATE -Wall -Wextra -fcoroutines
    -Wno-maybe-uninitialized)
endif()

# --- tests (gtest) ---
option(ASIO_QJS_BUILD_TESTS "Build unit tests" ON)
if(ASIO_QJS_BUILD_TESTS)
  enable_testing()
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)

  function(add_qjs_gtest name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE GTest::gtest_main asio_qjs_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${name} PRIVATE -Wall -Wextra -fcoroutines -O2
        -Wno-maybe-uninitialized)
    endif()
    gtest_discover_tests(${name}
      PROPERTIES
        TIMEOUT 60
    )
  endfunction()

  add_qjs_gtest(test_function_registry tests/test_function_registry.cpp)
  add_qjs_gtest(test_channel tests/test_channel.cpp)
  add_qjs_gtest(test_two_qjs tests/test_two_qjs.cpp)
  add_qjs_gtest(test_fetch tests/test_fetch.cpp)
  add_qjs_gtest(test_json tests/test_json.cpp)
  add_qjs_gtest(test_host_id tests/test_host_id.cpp)

  add_executable(test_wpt_fetch tests/test_wpt_fetch.cpp)
  target_link_libraries(test_wpt_fetch PRIVATE GTest::gtest_main asio_qjs_core)
  target_compile_definitions(test_wpt_fetch PRIVATE
    "ASIO_QJS_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(test_wpt_fetch PRIVATE -Wall -Wextra -fcoroutines -O2
      -Wno-maybe-uninitialized)
  endif()
  gtest_discover_tests(test_wpt_fetch PROPERTIES TIMEOUT 120)
endif()
