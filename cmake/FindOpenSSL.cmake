# Custom FindOpenSSL shim that redirects curl's OpenSSL dependency to the
# LibreSSL we just built from source. This file must be found by CMake before
# the built-in FindOpenSSL module, so the top-level CMakeLists.txt adds
# ${CMAKE_SOURCE_DIR}/cmake to CMAKE_MODULE_PATH before fetching curl.

if(NOT LIBRESSL_INSTALL_DIR)
  message(FATAL_ERROR "LIBRESSL_INSTALL_DIR is not set")
endif()

find_library(OPENSSL_SSL_LIBRARY
  NAMES ssl libssl
  HINTS "${LIBRESSL_INSTALL_DIR}/lib"
  NO_DEFAULT_PATH
)
find_library(OPENSSL_CRYPTO_LIBRARY
  NAMES crypto libcrypto
  HINTS "${LIBRESSL_INSTALL_DIR}/lib"
  NO_DEFAULT_PATH
)
find_path(OPENSSL_INCLUDE_DIR
  NAMES openssl/ssl.h
  HINTS "${LIBRESSL_INSTALL_DIR}/include"
  NO_DEFAULT_PATH
)

if(NOT OPENSSL_SSL_LIBRARY OR NOT OPENSSL_CRYPTO_LIBRARY OR NOT OPENSSL_INCLUDE_DIR)
  message(FATAL_ERROR "Could not find LibreSSL libraries in ${LIBRESSL_INSTALL_DIR}")
endif()

set(OPENSSL_FOUND TRUE)
set(OPENSSL_VERSION "1.1.1" CACHE STRING "OpenSSL version exposed to curl" FORCE)
set(OPENSSL_VERSION_MAJOR 1)
set(OPENSSL_VERSION_MINOR 1)
set(OPENSSL_VERSION_PATCH 1)
set(OPENSSL_LIBRARIES "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}")

if(NOT TARGET OpenSSL::Crypto)
  add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
  set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
  )
endif()

if(NOT TARGET OpenSSL::SSL)
  add_library(OpenSSL::SSL UNKNOWN IMPORTED)
  set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
  )
endif()
