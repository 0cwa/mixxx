# Prepared by ChatGPT for human review.
cmake_minimum_required(VERSION 3.20)
project(MixxxRekordboxDatabaseReaders LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(Git REQUIRED)

set(
  LIBDJINTEROP_SOURCE_DIR
  ""
  CACHE PATH
  "Pinned and patched libdjinterop checkout"
)
set(QUALIFIED_LIBDJINTEROP_REVISION d61727c5a3369f79d11b70afba6c925ddeb7f54c)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${LIBDJINTEROP_SOURCE_DIR}" rev-parse HEAD
  RESULT_VARIABLE REVISION_RESULT
  OUTPUT_VARIABLE ACTUAL_LIBDJINTEROP_REVISION
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(
  NOT REVISION_RESULT EQUAL 0
  OR NOT ACTUAL_LIBDJINTEROP_REVISION STREQUAL QUALIFIED_LIBDJINTEROP_REVISION
)
  message(FATAL_ERROR "Use the exact qualified libdjinterop checkout")
endif()

option(
  REKORDBOX_DATABASE_WITH_SQLCIPHER
  "Enable qualified encrypted database profiles"
  ON
)
option(
  REKORDBOX_DATABASE_SANITIZERS
  "Instrument adapter/snapshot tests with ASan and UBSan"
  OFF
)
option(
  REKORDBOX_DATABASE_QT_ISOLATION_TEST
  "Exercise direct SQLite calls on Qt-owned handles"
  OFF
)
get_filename_component(
  MIXXX_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
  ABSOLUTE
)
set(REKORDBOX_SOURCE "${MIXXX_ROOT}/src/library/rekordbox")
set(REKORDBOX_TEST "${MIXXX_ROOT}/src/test")

if(REKORDBOX_DATABASE_WITH_SQLCIPHER)
  execute_process(
    COMMAND
      "${GIT_EXECUTABLE}" -C "${LIBDJINTEROP_SOURCE_DIR}" diff --binary HEAD
    RESULT_VARIABLE PATCH_RESULT
    OUTPUT_VARIABLE ACTUAL_LIBDJINTEROP_PATCH
  )
  string(SHA256 ACTUAL_LIBDJINTEROP_PATCH_HASH "${ACTUAL_LIBDJINTEROP_PATCH}")
  file(
    SHA256
    "${CMAKE_CURRENT_SOURCE_DIR}/../onelibrary_probe/patches/libdjinterop-private-snapshot.patch"
    QUALIFIED_LIBDJINTEROP_PATCH_HASH
  )
  if(
    NOT PATCH_RESULT EQUAL 0
    OR
      NOT
        ACTUAL_LIBDJINTEROP_PATCH_HASH
          STREQUAL
          QUALIFIED_LIBDJINTEROP_PATCH_HASH
  )
    message(
      FATAL_ERROR
      "Apply exactly the supplied libdjinterop private-snapshot patch"
    )
  endif()

  function(add_private_djinterop)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Private dependency archives" FORCE)
    set(
      EXPERIMENTAL_ENABLE_SQLCIPHER
      ON
      CACHE BOOL
      "Qualified encrypted reader"
      FORCE
    )
    set(
      SYSTEM_SQLITE
      OFF
      CACHE BOOL
      "Use the qualified private SQLCipher"
      FORCE
    )
    set(BUILD_EXAMPLES OFF CACHE BOOL "Build only the dependency library" FORCE)
    set(BUILD_DOC OFF CACHE BOOL "Build only the dependency library" FORCE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
    set(CMAKE_DISABLE_FIND_PACKAGE_Boost TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE)
    set(CMAKE_PROJECT_VERSION 0.28.0)
    add_subdirectory(
      "${LIBDJINTEROP_SOURCE_DIR}"
      "private-djinterop"
      EXCLUDE_FROM_ALL
    )
  endfunction()
  add_private_djinterop()
  set_target_properties(
    DjInterop
    sqlcipher
    PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      C_VISIBILITY_PRESET hidden
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
  )
  set(REKORDBOX_SQLITE_TARGET sqlcipher)
  set(REKORDBOX_SQLITE_INCLUDE "${LIBDJINTEROP_SOURCE_DIR}/ext/sqlcipher")
else()
  add_library(
    rekordbox-plain-sqlite
    STATIC
    "${LIBDJINTEROP_SOURCE_DIR}/ext/sqlite-amalgamation/sqlite3.c"
  )
  target_include_directories(
    rekordbox-plain-sqlite
    PUBLIC "${LIBDJINTEROP_SOURCE_DIR}/ext/sqlite-amalgamation"
  )
  target_compile_definitions(
    rekordbox-plain-sqlite
    PRIVATE SQLITE_TEMP_STORE=2 SQLITE_OMIT_LOAD_EXTENSION
  )
  set_target_properties(
    rekordbox-plain-sqlite
    PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden
  )
  set(REKORDBOX_SQLITE_TARGET rekordbox-plain-sqlite)
  set(
    REKORDBOX_SQLITE_INCLUDE
    "${LIBDJINTEROP_SOURCE_DIR}/ext/sqlite-amalgamation"
  )
endif()

set(
  READER_SOURCES
  "${REKORDBOX_SOURCE}/rekordboxdatabasesnapshot.cpp"
  "${REKORDBOX_SOURCE}/rekordboxnativereader.cpp"
  "${REKORDBOX_SOURCE}/rekordboxdatabasesource.cpp"
)
if(REKORDBOX_DATABASE_WITH_SQLCIPHER)
  list(
    APPEND
    READER_SOURCES
    "${REKORDBOX_SOURCE}/rekordboxonelibraryreader.cpp"
  )
endif()

function(configure_validation_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(${target} PUBLIC "${MIXXX_ROOT}/src")
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    if(REKORDBOX_DATABASE_SANITIZERS)
      target_compile_options(
        ${target}
        PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
      )
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

function(configure_reader_target target)
  configure_validation_target(${target})
  target_link_libraries(
    ${target}
    PRIVATE
      ${REKORDBOX_SQLITE_TARGET}
      OpenSSL::Crypto
      Threads::Threads
      ${CMAKE_DL_LIBS}
  )
  if(REKORDBOX_DATABASE_WITH_SQLCIPHER)
    target_compile_definitions(${target} PRIVATE SQLITE_HAS_CODEC)
    target_link_libraries(${target} PRIVATE DjInterop)
  else()
    target_compile_definitions(${target} PRIVATE REKORDBOX_NO_ONELIBRARY)
  endif()
endfunction()

# Only this shared boundary is a proposed application dependency. The private
# static test library below must never be linked into the Mixxx process.
add_library(rekordbox-database-readers SHARED ${READER_SOURCES})
configure_reader_target(rekordbox-database-readers)
target_compile_definitions(
  rekordbox-database-readers
  PRIVATE REKORDBOX_DATABASE_BUILD_SHARED
  INTERFACE REKORDBOX_DATABASE_USE_SHARED
)
set_target_properties(
  rekordbox-database-readers
  PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
)
if(APPLE)
  file(
    WRITE
    "${CMAKE_CURRENT_BINARY_DIR}/reader-exports.txt"
    "__ZN5mixxx9rekordbox18readDatabaseSourceE*\n"
  )
  target_link_options(
    rekordbox-database-readers
    PRIVATE
      "LINKER:-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/reader-exports.txt"
  )
elseif(UNIX)
  file(
    WRITE
    "${CMAKE_CURRENT_BINARY_DIR}/reader-exports.map"
    "{ global: _ZN5mixxx9rekordbox18readDatabaseSourceE*; local: *; };\n"
  )
  target_link_options(
    rekordbox-database-readers
    PRIVATE
      "LINKER:--exclude-libs,ALL"
      "LINKER:--version-script,${CMAKE_CURRENT_BINARY_DIR}/reader-exports.map"
  )
endif()

add_library(rekordbox-database-test-core STATIC ${READER_SOURCES})
configure_reader_target(rekordbox-database-test-core)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")

function(database_test target source)
  add_executable(${target} "${source}")
  configure_validation_target(${target})
  target_include_directories(${target} PRIVATE "${REKORDBOX_SQLITE_INCLUDE}")
  if(REKORDBOX_DATABASE_WITH_SQLCIPHER)
    target_compile_definitions(${target} PRIVATE SQLITE_HAS_CODEC)
  else()
    target_compile_definitions(${target} PRIVATE REKORDBOX_NO_ONELIBRARY)
  endif()
  target_link_libraries(${target} PRIVATE rekordbox-database-test-core)
endfunction()

function(shared_consumer target source)
  add_executable(${target} "${source}")
  configure_validation_target(${target})
  target_compile_definitions(
    ${target}
    PRIVATE REKORDBOX_DATABASE_SHARED_CONSUMER
  )
  if(NOT REKORDBOX_DATABASE_WITH_SQLCIPHER)
    target_compile_definitions(${target} PRIVATE REKORDBOX_NO_ONELIBRARY)
  endif()
  target_include_directories(
    ${target}
    PRIVATE "${LIBDJINTEROP_SOURCE_DIR}/ext/sqlite-amalgamation"
  )
  target_link_libraries(
    ${target}
    PRIVATE rekordbox-database-readers "${REKORDBOX_SYSTEM_SQLITE}"
  )
endfunction()

find_library(REKORDBOX_SYSTEM_SQLITE NAMES sqlite3 libsqlite3.so.0 REQUIRED)

add_library(
  rekordbox-test-anlz-runtime
  STATIC
  "${MIXXX_ROOT}/lib/rekordbox-metadata/rekordbox_anlz.cpp"
  "${MIXXX_ROOT}/lib/kaitai/kaitai/kaitaistream.cpp"
)
target_include_directories(
  rekordbox-test-anlz-runtime
  SYSTEM
  PUBLIC "${MIXXX_ROOT}/lib/rekordbox-metadata" "${MIXXX_ROOT}/lib/kaitai"
)
target_compile_definitions(
  rekordbox-test-anlz-runtime
  PRIVATE KS_STR_ENCODING_NONE
)
target_link_libraries(rekordbox-test-anlz-runtime PRIVATE ZLIB::ZLIB)

database_test(rekordbox-database-snapshot-test "${REKORDBOX_TEST}/rekordboxdatabasesnapshot_standalone_test.cpp")
add_test(
  NAME rekordbox-database-snapshot
  COMMAND
    rekordbox-database-snapshot-test
    "${CMAKE_CURRENT_BINARY_DIR}/snapshot-cases"
)

database_test(rekordbox-native-reader-test "${REKORDBOX_TEST}/rekordbox_native_reader_test.cpp")
add_test(
  NAME rekordbox-native-reader
  COMMAND
    rekordbox-native-reader-test
    "${REKORDBOX_TEST}/rekordboxfixtures/native-db6000/schema.sql"
)

database_test(rekordbox-database-source-test "${REKORDBOX_TEST}/rekordboxdatabasesource_standalone_test.cpp")
target_sources(
  rekordbox-database-source-test
  PRIVATE "${REKORDBOX_SOURCE}/rekordboxanlzdecoder.cpp"
)
target_link_libraries(
  rekordbox-database-source-test
  PRIVATE rekordbox-test-anlz-runtime
)
add_test(
  NAME rekordbox-database-source
  COMMAND
    rekordbox-database-source-test
    "${REKORDBOX_TEST}/rekordboxfixtures/database/onelibrary-db1000.sql"
    "${REKORDBOX_TEST}/rekordboxfixtures/native-db6000/schema.sql"
    "${REKORDBOX_TEST}/rekordboxfixtures/anlz-synthetic/ANLZ0000.DAT"
    "${CMAKE_CURRENT_BINARY_DIR}/source-cases"
)
shared_consumer(rekordbox-database-shared-consumer "${REKORDBOX_TEST}/rekordboxdatabasesource_standalone_test.cpp")
target_sources(
  rekordbox-database-shared-consumer
  PRIVATE "${REKORDBOX_SOURCE}/rekordboxanlzdecoder.cpp"
)
target_link_libraries(
  rekordbox-database-shared-consumer
  PRIVATE rekordbox-test-anlz-runtime
)
add_test(
  NAME rekordbox-database-shared-consumer
  COMMAND
    rekordbox-database-shared-consumer
    "${REKORDBOX_TEST}/rekordboxfixtures/database/onelibrary-db1000.sql"
    "${REKORDBOX_TEST}/rekordboxfixtures/native-db6000/schema.sql"
    "${REKORDBOX_TEST}/rekordboxfixtures/anlz-synthetic/ANLZ0000.DAT"
    "${CMAKE_CURRENT_BINARY_DIR}/shared-consumer-cases"
)

if(REKORDBOX_DATABASE_QT_ISOLATION_TEST)
  find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Sql)
  find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Sql)
  target_compile_definitions(
    rekordbox-database-shared-consumer
    PRIVATE REKORDBOX_DATABASE_QT_CONSUMER
  )
  target_link_libraries(
    rekordbox-database-shared-consumer
    PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Sql
  )
endif()

if(REKORDBOX_DATABASE_WITH_SQLCIPHER)
  database_test(rekordbox-onelibrary-reader-test "${REKORDBOX_TEST}/rekordboxonelibraryreader_standalone_test.cpp")
  add_test(
    NAME rekordbox-onelibrary-reader
    COMMAND
      rekordbox-onelibrary-reader-test
      "${REKORDBOX_TEST}/rekordboxfixtures/database/onelibrary-db1000.sql"
      "${CMAKE_CURRENT_BINARY_DIR}/onelibrary-cases"
  )
  database_test(rekordbox-native-fixture-probe "${MIXXX_ROOT}/tools/rekordbox/native_fixture_probe.cpp")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
database_test(rekordbox-live-wal-driver "${CMAKE_CURRENT_SOURCE_DIR}/live_wal_driver.cpp")
add_test(
  NAME rekordbox-live-wal
  COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/live_wal_test.py"
    "$<TARGET_FILE:rekordbox-live-wal-driver>"
    "${CMAKE_CURRENT_BINARY_DIR}/live-wal-cases"
)

get_property(REKORDBOX_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(
  ${REKORDBOX_REGISTERED_TESTS}
  PROPERTIES ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/tmp"
)
# End of ChatGPT-prepared isolated build proposal.
