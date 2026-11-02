set(
  SIGNALSMITH_SOURCE
  AUTO
  CACHE STRING
  "Signalsmith dependency source: AUTO, SYSTEM or BUNDLED"
)
set_property(CACHE SIGNALSMITH_SOURCE PROPERTY STRINGS AUTO SYSTEM BUNDLED)
set(_signalsmith_sources AUTO SYSTEM BUNDLED)
if(NOT SIGNALSMITH_SOURCE IN_LIST _signalsmith_sources)
  message(FATAL_ERROR "SIGNALSMITH_SOURCE must be AUTO, SYSTEM or BUNDLED")
endif()
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
option(SIGNALSMITH_USE_PFFFT "Use PFFFT for Signalsmith Linear" OFF)
option(
  SIGNALSMITH_USE_PFFFT_DOUBLE
  "Use double-precision PFFFT for Signalsmith Linear"
  OFF
)

add_library(mixxx-signalsmith INTERFACE)
if(SIGNALSMITH_SOURCE STREQUAL "SYSTEM")
  find_package(Signalsmith 1.3.2 REQUIRED)
elseif(SIGNALSMITH_SOURCE STREQUAL "AUTO")
  find_package(Signalsmith 1.3.2 QUIET)
endif()
if(NOT SIGNALSMITH_SOURCE STREQUAL "BUNDLED" AND Signalsmith_FOUND)
  message(STATUS "Signalsmith: using installed Stretch ${Signalsmith_VERSION}")
  target_link_libraries(mixxx-signalsmith INTERFACE Signalsmith::Stretch)
else()
  message(
    STATUS
    "Signalsmith: using bundled Stretch 57b93f4e9206a089a45387eaa39bdc9f310d3308 and Linear 5668673560146a9cfe38c25315071e3fd68c8317"
  )
  target_include_directories(
    mixxx-signalsmith
    SYSTEM
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/../signalsmith-stretch"
      "${CMAKE_CURRENT_SOURCE_DIR}/../signalsmith-linear/include"
  )
endif()

if(MSVC)
  target_compile_options(mixxx-signalsmith INTERFACE /bigobj)
endif()
if(APPLE AND SIGNALSMITH_USE_ACCELERATE)
  target_link_libraries(mixxx-signalsmith INTERFACE "-framework Accelerate")
  target_compile_definitions(
    mixxx-signalsmith
    INTERFACE SIGNALSMITH_USE_ACCELERATE ACCELERATE_NEW_LAPACK
  )
endif()
if(SIGNALSMITH_USE_IPP)
  find_package(IPP REQUIRED)
  target_link_libraries(
    mixxx-signalsmith
    INTERFACE IPP::ippcore IPP::ipps IPP::ippi
  )
  target_compile_definitions(mixxx-signalsmith INTERFACE SIGNALSMITH_USE_IPP)
endif()
if(SIGNALSMITH_USE_PFFFT)
  target_compile_definitions(mixxx-signalsmith INTERFACE SIGNALSMITH_USE_PFFFT)
endif()
if(SIGNALSMITH_USE_PFFFT_DOUBLE)
  target_compile_definitions(
    mixxx-signalsmith
    INTERFACE SIGNALSMITH_USE_PFFFT_DOUBLE
  )
endif()

install(
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/../signalsmith-stretch/LICENSE.txt"
  DESTINATION "${MIXXX_INSTALL_LICENSEDIR}/signalsmith-stretch"
)
install(
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/../signalsmith-linear/LICENSE.txt"
  DESTINATION "${MIXXX_INSTALL_LICENSEDIR}/signalsmith-linear"
)
