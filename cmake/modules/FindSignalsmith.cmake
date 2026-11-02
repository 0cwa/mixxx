find_path(
  Signalsmith_STRETCH_INCLUDE_DIR
  NAMES signalsmith-stretch.h
  PATH_SUFFIXES signalsmith-stretch
)
find_path(Signalsmith_LINEAR_INCLUDE_DIR NAMES signalsmith-linear/stft.h)
mark_as_advanced(Signalsmith_STRETCH_INCLUDE_DIR Signalsmith_LINEAR_INCLUDE_DIR)

unset(Signalsmith_VERSION)
if(EXISTS "${Signalsmith_STRETCH_INCLUDE_DIR}/signalsmith-stretch.h")
  file(
    STRINGS
    "${Signalsmith_STRETCH_INCLUDE_DIR}/signalsmith-stretch.h"
    _signalsmith_version_line
    REGEX "static constexpr size_t version\\[3\\]"
  )
  string(
    REGEX MATCH
    "\\{[ \t]*([0-9]+),[ \t]*([0-9]+),[ \t]*([0-9]+)[ \t]*\\}"
    _signalsmith_version_match
    "${_signalsmith_version_line}"
  )
  if(_signalsmith_version_match)
    set(
      Signalsmith_VERSION
      "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}"
    )
  endif()
endif()

if(Signalsmith_STRETCH_INCLUDE_DIR AND Signalsmith_LINEAR_INCLUDE_DIR)
  include(CMakePushCheckState)
  include(CheckCXXSourceCompiles)
  cmake_push_check_state(RESET)
  set(
    CMAKE_REQUIRED_INCLUDES
    "${Signalsmith_STRETCH_INCLUDE_DIR};${Signalsmith_LINEAR_INCLUDE_DIR}"
  )
  if(MSVC)
    set(CMAKE_REQUIRED_FLAGS "/bigobj")
  endif()
  unset(Signalsmith_API_COMPATIBLE CACHE)
  check_cxx_source_compiles(
    "#include <cstring>
#include <signalsmith-stretch.h>
int main() {
    signalsmith::stretch::SignalsmithStretch<float> stretch(0);
    stretch.presetDefault(2, 48000.0f);
    stretch.setTransposeFactor(1.0f);
    stretch.setFormantFactor(1.0f);
    float input[2][64]{};
    float output[2][64]{};
    float* inputChannels[]{input[0], input[1]};
    float* outputChannels[]{output[0], output[1]};
    stretch.outputSeek(inputChannels, 64);
    stretch.process(inputChannels, 64, outputChannels, 64);
    stretch.reset();
    return stretch.inputLatency() + stretch.outputLatency() == 0;
}"
    Signalsmith_API_COMPATIBLE
  )
  cmake_pop_check_state()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Signalsmith
  REQUIRED_VARS
    Signalsmith_STRETCH_INCLUDE_DIR
    Signalsmith_LINEAR_INCLUDE_DIR
    Signalsmith_API_COMPATIBLE
  VERSION_VAR Signalsmith_VERSION
)

if(Signalsmith_FOUND AND NOT TARGET Signalsmith::Stretch)
  add_library(Signalsmith::Stretch INTERFACE IMPORTED)
  set_target_properties(
    Signalsmith::Stretch
    PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES
        "${Signalsmith_STRETCH_INCLUDE_DIR};${Signalsmith_LINEAR_INCLUDE_DIR}"
  )
endif()
