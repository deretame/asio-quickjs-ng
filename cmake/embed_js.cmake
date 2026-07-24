# Generate src/js_embedded.hpp from the JS polyfills.
# Run from CMakeLists.txt via:
#   cmake -DOUTPUT_DIR=<dir> -P cmake/embed_js.cmake
#
# OUTPUT_DIR must be set to the directory where js_embedded.hpp will be written.

set(JS_FILES
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/abort.js"
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/text-encoding-polyfill.js"
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/whatwg-url-polyfill.js"
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/body_polyfill.js"
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/buffer.js"
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/headers.js"
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/request.js"
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/response.js"
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/fetch.js"
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/crypto.js"
  "${CMAKE_CURRENT_LIST_DIR}/../src/js/http.js"
)

# Explicit filename -> C++ variable name mapping so that fetch.cpp and the
# generated header always agree, regardless of how the file is named.
set(JS_VAR_NAME_abort.js "kJsAbortBytes")
set(JS_VAR_NAME_text-encoding-polyfill.js "kJsTextEncodingPolyfillBytes")
set(JS_VAR_NAME_whatwg-url-polyfill.js "kJsWhatwgUrlPolyfillBytes")
set(JS_VAR_NAME_body_polyfill.js "kJsBodyPolyfillBytes")
set(JS_VAR_NAME_buffer.js "kJsBufferBytes")
set(JS_VAR_NAME_headers.js "kJsHeadersBytes")
set(JS_VAR_NAME_request.js "kJsRequestBytes")
set(JS_VAR_NAME_response.js "kJsResponseBytes")
set(JS_VAR_NAME_fetch.js "kJsFetchBytes")
set(JS_VAR_NAME_crypto.js "kJsCryptoBytes")
set(JS_VAR_NAME_http.js "kJsHttpBytes")

set(OUTPUT_HEADER "${OUTPUT_DIR}/js_embedded.hpp")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(HEADER_CONTENT "#pragma once\n\n#include <cstddef>\n\nnamespace js_embedded {\n")

foreach(JS_PATH IN LISTS JS_FILES)
  if(NOT EXISTS "${JS_PATH}")
    message(FATAL_ERROR "Missing JS file: ${JS_PATH}")
  endif()

  get_filename_component(JS_NAME "${JS_PATH}" NAME)
  set(VAR_NAME "${JS_VAR_NAME_${JS_NAME}}")
  if(NOT VAR_NAME)
    message(FATAL_ERROR "No variable name mapping for ${JS_NAME}")
  endif()

  # Read file as hex, e.g. "48656C6C6F"
  file(READ "${JS_PATH}" HEX_CONTENT HEX)
  string(LENGTH "${HEX_CONTENT}" HEX_LEN)

  string(APPEND HEADER_CONTENT "\ninline constexpr unsigned char ${VAR_NAME}[] = {\n")

  set(ARRAY_CONTENT "")
  set(HEX_IDX 0)
  while(HEX_IDX LESS HEX_LEN)
    string(SUBSTRING "${HEX_CONTENT}" ${HEX_IDX} 2 BYTE_HEX)
    string(APPEND ARRAY_CONTENT " 0x${BYTE_HEX},")
    math(EXPR HEX_IDX "${HEX_IDX} + 2")
  endwhile()

  string(APPEND HEADER_CONTENT "${ARRAY_CONTENT}\n};\n")
endforeach()

string(APPEND HEADER_CONTENT "\n} // namespace js_embedded\n")

file(WRITE "${OUTPUT_HEADER}" "${HEADER_CONTENT}")
message(STATUS "Generated ${OUTPUT_HEADER}")
