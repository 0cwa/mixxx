# Prepared by ChatGPT for human review.
cmake_minimum_required(VERSION 3.20)
project(MixxxOneLibraryProbe LANGUAGES CXX)

set(
  LIBDJINTEROP_SOURCE_DIR
  ""
  CACHE PATH
  "Checkout of the qualified libdjinterop revision"
)
set(QUALIFIED_LIBDJINTEROP_REVISION d61727c5a3369f79d11b70afba6c925ddeb7f54c)
find_package(Git REQUIRED)
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
  message(
    FATAL_ERROR
    "LIBDJINTEROP_SOURCE_DIR must be the qualified, pinned checkout"
  )
endif()

find_package(DjInterop 0.28.0 EXACT CONFIG REQUIRED)
add_executable(rekordbox-onelibrary-probe main.cpp)
target_compile_features(rekordbox-onelibrary-probe PRIVATE cxx_std_20)
target_link_libraries(rekordbox-onelibrary-probe PRIVATE DjInterop::DjInterop)
target_compile_definitions(
  rekordbox-onelibrary-probe
  PRIVATE
    PROBE_SCHEMA_DIRECTORY="${LIBDJINTEROP_SOURCE_DIR}/testdata/ref/onelibrary/v1"
    PROBE_HAS_SQLCIPHER=$<BOOL:${DJINTEROP_EXPERIMENTAL_ENABLE_SQLCIPHER}>
)

option(
  PROBE_WITH_SYSTEM_SQLITE
  "Exercise an ordinary SQLite in the same process"
  ON
)
if(PROBE_WITH_SYSTEM_SQLITE)
  find_library(PROBE_SQLITE_LIBRARY NAMES sqlite3 libsqlite3.so.0 REQUIRED)
  target_include_directories(
    rekordbox-onelibrary-probe
    PRIVATE "${LIBDJINTEROP_SOURCE_DIR}/ext/sqlite-amalgamation"
  )
  target_link_libraries(
    rekordbox-onelibrary-probe
    PRIVATE "${PROBE_SQLITE_LIBRARY}"
  )
  target_compile_definitions(
    rekordbox-onelibrary-probe
    PRIVATE PROBE_WITH_SYSTEM_SQLITE=1
  )
endif()

option(
  PROBE_WITH_QT_SQLITE
  "Exercise the QSQLITE plugin beside libdjinterop"
  OFF
)
if(PROBE_WITH_QT_SQLITE)
  find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Sql)
  find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Sql)
  target_link_libraries(
    rekordbox-onelibrary-probe
    PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Sql
  )
  target_compile_definitions(
    rekordbox-onelibrary-probe
    PRIVATE PROBE_WITH_QT_SQLITE=1
  )
endif()

include(CTest)
add_test(
  NAME rekordbox-onelibrary-dependency
  COMMAND rekordbox-onelibrary-probe
)
# End of ChatGPT-prepared build instructions.
