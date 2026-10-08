# Standalone QtCore regression with unchanged scheduler/worker sources.
# Configure this directory directly. It requires no Mixxx, audio or GUI launch.
cmake_minimum_required(VERSION 3.20)
project(EngineWorkerSchedulerRegression LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
get_filename_component(
    default_mixxx_source_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE
)
set(
    MIXXX_SOURCE_ROOT
    "${default_mixxx_source_root}"
    CACHE PATH "Mixxx checkout containing the actual scheduler sources"
)
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(GTest REQUIRED)
add_executable(
    engineworkerscheduler-regression
    engineworkerscheduler_regression.cpp
    "${MIXXX_SOURCE_ROOT}/src/engine/engineworker.cpp"
    "${MIXXX_SOURCE_ROOT}/src/engine/engineworker.h"
    "${MIXXX_SOURCE_ROOT}/src/engine/engineworkerscheduler.cpp"
    "${MIXXX_SOURCE_ROOT}/src/engine/engineworkerscheduler.h"
)
# Only this target shadows Event tracing and assertion support. QtCore
# synchronization, EngineWorker and EngineWorkerScheduler use the real sources.
target_include_directories(
    engineworkerscheduler-regression
    PRIVATE test_stubs "${MIXXX_SOURCE_ROOT}/src"
)
target_link_libraries(
    engineworkerscheduler-regression PRIVATE Qt6::Core GTest::gtest_main
)
enable_testing()
add_test(
    NAME engineworkerscheduler-retained-notification
    COMMAND engineworkerscheduler-regression
)
set_tests_properties(
    engineworkerscheduler-retained-notification PROPERTIES TIMEOUT 20
)
