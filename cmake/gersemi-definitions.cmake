# Definitions for project-specific commands used only by gersemi.

function(fatal_error_missing_env)
  cmake_parse_arguments(FATAL_ERROR_MISSING_ENV "" "" "" ${ARGN})
endfunction()

function(default_option)
  cmake_parse_arguments(DEFAULT_OPTION "" "" "" ${ARGN})
endfunction()

function(mixxx_target_warnings_fatal)
  cmake_parse_arguments(MIXXX_TARGET_WARNINGS_FATAL "" "" "" ${ARGN})
endfunction()

function(is_static_library)
  cmake_parse_arguments(IS_STATIC_LIBRARY "" "" "" ${ARGN})
endfunction()
