cmake_minimum_required(VERSION 3.28)
project(NessStudioSqlCipherRuntime LANGUAGES C)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This pinned product runtime recipe is Windows MSVC x64 only")
endif()
foreach(required SQLCIPHER_AMALGAMATION OPENSSL_SOURCE_DIR OPENSSL_BUILD_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing ${required}; see the pinned runtime build instructions")
  endif()
endforeach()
add_library(e_sqlcipher SHARED "${SQLCIPHER_AMALGAMATION}/sqlite3.c" win32_compat.c)
set_property(TARGET e_sqlcipher PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
target_include_directories(e_sqlcipher PRIVATE
  "${OPENSSL_BUILD_DIR}/include" "${OPENSSL_SOURCE_DIR}/include")
target_compile_definitions(e_sqlcipher PRIVATE
  "SQLITE_API=__declspec(dllexport)"
  SQLITE_HAS_CODEC SQLCIPHER_CRYPTO_OPENSSL
  SQLITE_EXTRA_INIT=sqlcipher_extra_init SQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown
  SQLITE_TEMP_STORE=2 SQLITE_THREADSAFE=1 SQLITE_ENABLE_API_ARMOR
  SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_DEFAULT_RECURSIVE_TRIGGERS=1
  SQLITE_ENABLE_COLUMN_METADATA SQLITE_ENABLE_FTS3 SQLITE_ENABLE_FTS3_PARENTHESIS
  SQLITE_ENABLE_FTS4 SQLITE_ENABLE_FTS5 SQLITE_ENABLE_RTREE SQLITE_ENABLE_SNAPSHOT
  SQLITE_MAX_VARIABLE_NUMBER=32766 SQLITE_MAX_MMAP_SIZE=0x7fff0000
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS _WIN32_WINNT=0x0A00)
target_compile_options(e_sqlcipher PRIVATE /O2 /W4 /sdl /guard:cf)
target_link_options(e_sqlcipher PRIVATE /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf /Brepro)
target_link_libraries(e_sqlcipher PRIVATE "${OPENSSL_BUILD_DIR}/libcrypto.lib"
  ws2_32 crypt32 advapi32 user32 gdi32 bcrypt)
