# Generate hono_embedded.hpp from the bundled Hono source.
# Run from CMakeLists.txt via:
#   cmake -DOUTPUT_DIR=<dir> -P cmake/embed_hono.cmake

set(HONO_BUNDLE_PATH "${CMAKE_CURRENT_LIST_DIR}/../src/js/hono.bundle.js")

if(NOT EXISTS "${HONO_BUNDLE_PATH}")
  message(FATAL_ERROR "Missing Hono bundle: ${HONO_BUNDLE_PATH}")
endif()

set(OUTPUT_HEADER "${OUTPUT_DIR}/hono_embedded.hpp")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

file(READ "${HONO_BUNDLE_PATH}" HEX_CONTENT HEX)
string(LENGTH "${HEX_CONTENT}" HEX_LEN)

set(HEADER_CONTENT "#pragma once\n\n#include <cstddef>\n\nnamespace hono_embedded {\n\n")
string(APPEND HEADER_CONTENT "inline constexpr unsigned char kHonoJsBytes[] = {\n")

set(ARRAY_CONTENT "")
set(HEX_IDX 0)
while(HEX_IDX LESS HEX_LEN)
  string(SUBSTRING "${HEX_CONTENT}" ${HEX_IDX} 2 BYTE_HEX)
  string(APPEND ARRAY_CONTENT " 0x${BYTE_HEX},")
  math(EXPR HEX_IDX "${HEX_IDX} + 2")
endwhile()

string(APPEND HEADER_CONTENT "${ARRAY_CONTENT}\n};\n\n")
string(APPEND HEADER_CONTENT "inline constexpr std::size_t kHonoJsSize = sizeof(kHonoJsBytes);\n\n")
string(APPEND HEADER_CONTENT "} // namespace hono_embedded\n")

file(WRITE "${OUTPUT_HEADER}" "${HEADER_CONTENT}")
message(STATUS "Generated ${OUTPUT_HEADER} from ${HONO_BUNDLE_PATH}")
