get_filename_component(
  _signalsmith_prefix
  "${CMAKE_CURRENT_LIST_DIR}/../../.."
  ABSOLUTE
)

if(NOT TARGET signalsmith-linear)
  add_library(signalsmith-linear INTERFACE IMPORTED)
  set_target_properties(
    signalsmith-linear
    PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES
        "${_signalsmith_prefix}/lib/signalsmith-linear/include"
  )
endif()

if(NOT TARGET signalsmith-stretch)
  add_library(signalsmith-stretch INTERFACE IMPORTED)
  set_target_properties(
    signalsmith-stretch
    PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES
        "${_signalsmith_prefix}/lib/signalsmith-stretch"
      INTERFACE_LINK_LIBRARIES signalsmith-linear
  )
endif()

if(NOT TARGET Signalsmith::Linear)
  add_library(Signalsmith::Linear ALIAS signalsmith-linear)
endif()
if(NOT TARGET Signalsmith::Stretch)
  add_library(Signalsmith::Stretch ALIAS signalsmith-stretch)
endif()

set(Signalsmith_FOUND TRUE)
