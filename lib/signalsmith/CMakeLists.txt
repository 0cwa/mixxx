cmake_minimum_required(VERSION 3.22)

add_library(signalsmith-linear INTERFACE)
target_include_directories(
  signalsmith-linear
  SYSTEM
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../signalsmith-linear/include>
)

# Linear's upstream CMake wrapper exposes these options. Keep them local to the
# interface target so the vendored headers remain usable without a sub-build or
# a network-capable package manager.
option(
  SIGNALSMITH_USE_ACCELERATE
  "Use Apple's Accelerate framework for Signalsmith Linear"
  ON
)
option(
  SIGNALSMITH_USE_IPP
  "Use Intel Performance Primitives for Signalsmith Linear"
  OFF
)
option(
  SIGNALSMITH_USE_PFFFT
  "Use PFFFT for Signalsmith Linear"
  OFF
)
option(
  SIGNALSMITH_USE_PFFFT_DOUBLE
  "Use double-precision PFFFT for Signalsmith Linear"
  OFF
)

if(MSVC)
  target_compile_options(signalsmith-linear INTERFACE /bigobj)
endif()

if(APPLE AND SIGNALSMITH_USE_ACCELERATE)
  target_link_libraries(signalsmith-linear INTERFACE "-framework Accelerate")
  target_compile_definitions(
    signalsmith-linear
    INTERFACE SIGNALSMITH_USE_ACCELERATE ACCELERATE_NEW_LAPACK
  )
endif()

if(SIGNALSMITH_USE_IPP)
  find_package(IPP REQUIRED)
  target_link_libraries(
    signalsmith-linear
    INTERFACE IPP::ippcore IPP::ipps IPP::ippi
  )
  target_compile_definitions(signalsmith-linear INTERFACE SIGNALSMITH_USE_IPP)
endif()

if(SIGNALSMITH_USE_PFFFT)
  target_compile_definitions(signalsmith-linear INTERFACE SIGNALSMITH_USE_PFFFT)
endif()

if(SIGNALSMITH_USE_PFFFT_DOUBLE)
  target_compile_definitions(
    signalsmith-linear
    INTERFACE SIGNALSMITH_USE_PFFFT_DOUBLE
  )
endif()

add_library(signalsmith-stretch INTERFACE)
target_include_directories(
  signalsmith-stretch
  SYSTEM
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../signalsmith-stretch>
)
target_link_libraries(signalsmith-stretch INTERFACE signalsmith-linear)
