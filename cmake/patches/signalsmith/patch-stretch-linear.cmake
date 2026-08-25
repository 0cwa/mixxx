if(
  NOT DEFINED MIXXX_SIGNALSMITH_STRETCH_CMAKE
  OR NOT DEFINED MIXXX_SIGNALSMITH_LINEAR_COMMIT
  OR NOT DEFINED MIXXX_SIGNALSMITH_LINEAR_URL_HASH
)
  message(FATAL_ERROR "Stretch and Linear patch parameters are required.")
endif()

if(NOT EXISTS "${MIXXX_SIGNALSMITH_STRETCH_CMAKE}")
  message(
    FATAL_ERROR
    "Stretch CMakeLists.txt was not found at ${MIXXX_SIGNALSMITH_STRETCH_CMAKE}."
  )
endif()

file(READ "${MIXXX_SIGNALSMITH_STRETCH_CMAKE}" _stretch_cmake)
set(
  _linear_repository
  "GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git"
)
set(_linear_tag "GIT_TAG 0.3.1")
set(_linear_shallow "GIT_SHALLOW ON")
foreach(_linear_token IN ITEMS _linear_repository _linear_tag _linear_shallow)
  string(FIND "${_stretch_cmake}" "${${_linear_token}}" _linear_token_offset)
  if(_linear_token_offset EQUAL -1)
    message(
      FATAL_ERROR
      "The pinned Stretch source no longer contains the expected Linear Git dependency."
    )
  endif()
endforeach()

string(
  CONCAT
  _linear_url
  "URL https://github.com/Signalsmith-Audio/linear/archive/"
  "${MIXXX_SIGNALSMITH_LINEAR_COMMIT}.tar.gz"
)
set(_linear_url_hash "URL_HASH SHA256=${MIXXX_SIGNALSMITH_LINEAR_URL_HASH}")
string(
  REPLACE
  "${_linear_repository}"
  "${_linear_url}"
  _patched_stretch_cmake
  "${_stretch_cmake}"
)
string(
  REPLACE
  "${_linear_tag}"
  "${_linear_url_hash}"
  _patched_stretch_cmake
  "${_patched_stretch_cmake}"
)
string(
  REPLACE
  "${_linear_shallow}"
  ""
  _patched_stretch_cmake
  "${_patched_stretch_cmake}"
)
file(WRITE "${MIXXX_SIGNALSMITH_STRETCH_CMAKE}" "${_patched_stretch_cmake}")
