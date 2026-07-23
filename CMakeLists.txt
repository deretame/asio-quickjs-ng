cmake_minimum_required(VERSION 3.23)

# Enforce the Ninja generator so build.ps1 and manual configure use the same backend.
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(FATAL_ERROR "This project requires the Ninja generator. Configure with: cmake -G Ninja ...")
endif()

project(asio_quickjs_ng LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- dependencies via vcpkg ---
find_package(spdlog CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(qjs CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(llhttp CONFIG REQUIRED)
find_package(async_simple CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS uuid)

# --- embedded JS polyfills: generate a header with byte arrays ---
set(JS_EMBED_FILES
  "${CMAKE_SOURCE_DIR}/src/js/abort.js"
  "${CMAKE_SOURCE_DIR}/src/js/text-encoding-polyfill.js"
  "${CMAKE_SOURCE_DIR}/src/js/whatwg-url-polyfill.js"
  "${CMAKE_SOURCE_DIR}/src/js/body_polyfill.js"
  "${CMAKE_SOURCE_DIR}/src/js/buffer.js"
  "${CMAKE_SOURCE_DIR}/src/js/headers.js"
  "${CMAKE_SOURCE_DIR}/src/js/request.js"
  "${CMAKE_SOURCE_DIR}/src/js/response.js"
  "${CMAKE_SOURCE_DIR}/src/js/fetch.js"
  "${CMAKE_SOURCE_DIR}/src/js/crypto.js"
)
set(JS_EMBED_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(JS_EMBED_HEADER "${JS_EMBED_HEADER_DIR}/js_embedded.hpp")

add_custom_command(
  OUTPUT "${JS_EMBED_HEADER}"
  COMMAND ${CMAKE_COMMAND} -DOUTPUT_DIR="${JS_EMBED_HEADER_DIR}" -P "${CMAKE_SOURCE_DIR}/cmake/embed_js.cmake"
  DEPENDS ${JS_EMBED_FILES}
  COMMENT "Generating js_embedded.hpp"
)
add_custom_target(js_embedded_header DEPENDS "${JS_EMBED_HEADER}")

# --- core library (host + fetch layers) ---
add_library(asio_qjs_core
  src/binary_store.cpp
  src/curl_runtime.cpp
  src/crypto.cpp
  src/host.cpp
  src/host_manager.cpp
  src/fetch.cpp
  src/function_registry.cpp
)
target_include_directories(asio_qjs_core PUBLIC "${CMAKE_SOURCE_DIR}/src" PRIVATE "${JS_EMBED_HEADER_DIR}")
target_compile_definitions(asio_qjs_core PUBLIC
  CURL_STATICLIB
  _WIN32_WINNT=0x0A00
  NOMINMAX
)
target_link_libraries(asio_qjs_core PUBLIC
  asio::asio
  qjs
  CURL::libcurl
  llhttp
  OpenSSL::SSL
  OpenSSL::Crypto
  spdlog::spdlog
  async_simple::async_simple_header_only
  nlohmann_json::nlohmann_json
  reflectcpp::reflectcpp
  Boost::uuid
)
if(WIN32)
  target_link_libraries(asio_qjs_core PUBLIC
    ws2_32
    mswsock
    bcrypt
    advapi32
    crypt32
    ntdll
  )
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(asio_qjs_core PRIVATE -Wall -Wextra -fcoroutines
    -Wno-maybe-uninitialized)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  target_compile_options(asio_qjs_core PRIVATE /W3 /permissive-)
endif()
add_dependencies(asio_qjs_core js_embedded_header)

add_executable(asio_qjs src/main.cpp)
target_link_libraries(asio_qjs PRIVATE asio_qjs_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(asio_qjs PRIVATE -Wall -Wextra -fcoroutines
    -Wno-maybe-uninitialized)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  target_compile_options(asio_qjs PRIVATE /W3 /permissive-)
endif()

# --- tests (gtest) ---
option(ASIO_QJS_BUILD_TESTS "Build unit tests" ON)
if(ASIO_QJS_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  include(GoogleTest)

  function(add_qjs_gtest name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE GTest::gtest_main asio_qjs_core)
    target_compile_definitions(${name} PRIVATE
      "ASIO_QJS_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${name} PRIVATE -Wall -Wextra -fcoroutines -O2
        -Wno-maybe-uninitialized)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
      target_compile_options(${name} PRIVATE /W3 /permissive-)
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
  add_qjs_gtest(test_host_manager tests/test_host_manager.cpp)
  add_qjs_gtest(test_base64 tests/test_base64.cpp)
  add_qjs_gtest(test_binary_store tests/test_binary_store.cpp)
  add_qjs_gtest(test_crypto tests/test_crypto.cpp)
  add_qjs_gtest(test_http tests/test_http.cpp)

  add_executable(test_wpt_fetch tests/test_wpt_fetch.cpp)
  target_link_libraries(test_wpt_fetch PRIVATE GTest::gtest_main asio_qjs_core)
  target_compile_definitions(test_wpt_fetch PRIVATE
    "ASIO_QJS_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(test_wpt_fetch PRIVATE -Wall -Wextra -fcoroutines -O2
      -Wno-maybe-uninitialized)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(test_wpt_fetch PRIVATE /W3 /permissive-)
  endif()
  gtest_discover_tests(test_wpt_fetch PROPERTIES TIMEOUT 120)
endif()
