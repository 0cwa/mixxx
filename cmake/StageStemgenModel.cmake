# Stage a verified Stemgen model without writing through the destination inode.
#
# COPY_FILE and file(RENAME ... RESULT) are available with the project minimum
# of CMake 3.22. Do not use RENAME NO_REPLACE here: CMake versions in the
# supported range have returned success while leaving the source directory
# entry behind in some cases. The temporary file is unique and lives beside
# the destination, so the ordinary same-filesystem rename publishes a complete
# file and replaces only the destination directory entry. This preserves any
# hard-linked sibling of a previous staged file.

function(
  _mixxx_stem_model_stage_path_has_symlink_component
  input_path
  result_var
)
  set(path_to_check "${input_path}")
  while(NOT path_to_check STREQUAL "")
    if(IS_SYMLINK "${path_to_check}")
      set(${result_var} TRUE PARENT_SCOPE)
      return()
    endif()
    get_filename_component(parent_path "${path_to_check}" DIRECTORY)
    if(parent_path STREQUAL path_to_check)
      break()
    endif()
    set(path_to_check "${parent_path}")
  endwhile()
  set(${result_var} FALSE PARENT_SCOPE)
endfunction()

function(
  _mixxx_stem_model_stage_file_matches
  input_path
  expected_size
  expected_sha256
  result_var
)
  set(${result_var} FALSE PARENT_SCOPE)
  if(
    IS_SYMLINK "${input_path}"
    OR NOT EXISTS "${input_path}"
    OR IS_DIRECTORY "${input_path}"
  )
    return()
  endif()
  file(SIZE "${input_path}" actual_size)
  if(NOT actual_size EQUAL expected_size)
    return()
  endif()
  file(SHA256 "${input_path}" actual_sha256)
  if(NOT actual_sha256 STREQUAL expected_sha256)
    return()
  endif()
  set(${result_var} TRUE PARENT_SCOPE)
endfunction()

function(
  mixxx_stage_stem_model
  source_path
  staged_path
  staged_directory
  expected_size
  expected_sha256
)
  if(CMAKE_VERSION VERSION_LESS "3.22")
    message(
      FATAL_ERROR
      "Stemgen model staging requires CMake 3.22 or newer; found ${CMAKE_VERSION}"
    )
  endif()

  set(source_has_symlink_component FALSE)
  _mixxx_stem_model_stage_path_has_symlink_component(
    "${source_path}"
    source_has_symlink_component
  )
  if(
    source_has_symlink_component
    OR IS_SYMLINK "${source_path}"
    OR NOT EXISTS "${source_path}"
    OR IS_DIRECTORY "${source_path}"
  )
    message(
      FATAL_ERROR
      "The Stemgen model source is not a regular file without symlink components: ${source_path}"
    )
  endif()

  set(staged_directory_has_symlink_component FALSE)
  _mixxx_stem_model_stage_path_has_symlink_component(
    "${staged_directory}"
    staged_directory_has_symlink_component
  )
  if(staged_directory_has_symlink_component)
    message(
      FATAL_ERROR
      "The staged Stemgen model path must not contain a symlink component: ${staged_directory}"
    )
  endif()
  file(MAKE_DIRECTORY "${staged_directory}")
  set(staged_directory_has_symlink_component FALSE)
  _mixxx_stem_model_stage_path_has_symlink_component(
    "${staged_directory}"
    staged_directory_has_symlink_component
  )
  if(staged_directory_has_symlink_component)
    message(
      FATAL_ERROR
      "The staged Stemgen model path contains a symlink component: ${staged_directory}"
    )
  endif()

  if(IS_SYMLINK "${staged_path}" OR IS_DIRECTORY "${staged_path}")
    message(
      FATAL_ERROR
      "The staged Stemgen model destination is not a regular file: ${staged_path}"
    )
  endif()

  get_filename_component(staged_name "${staged_path}" NAME)
  string(RANDOM LENGTH 32 ALPHABET "0123456789abcdef" staging_nonce)
  set(
    staging_temporary_path
    "${staged_directory}/.${staged_name}.${staging_nonce}.tmp"
  )
  if(
    EXISTS "${staging_temporary_path}"
    OR IS_SYMLINK "${staging_temporary_path}"
  )
    message(
      FATAL_ERROR
      "The temporary Stemgen model staging path already exists: ${staging_temporary_path}"
    )
  endif()

  # CMake 3.22 has no exclusive create primitive for a regular file. The
  # build directory is therefore a cooperative boundary: a concurrent build
  # must not use this staging directory. If the preflighted unique name is
  # occupied, fail rather than treating the entry as ours.
  set(staging_temporary_owned TRUE)
  file(
    COPY_FILE
    "${source_path}"
    "${staging_temporary_path}"
    RESULT staging_copy_result
  )
  if(NOT staging_copy_result STREQUAL "0")
    if(staging_temporary_owned)
      file(REMOVE "${staging_temporary_path}")
    endif()
    message(
      FATAL_ERROR
      "Could not stage the Stemgen model in a temporary file: ${staging_copy_result}"
    )
  endif()

  set(staging_temporary_matches FALSE)
  _mixxx_stem_model_stage_file_matches(
    "${staging_temporary_path}"
    "${expected_size}"
    "${expected_sha256}"
    staging_temporary_matches
  )
  if(NOT staging_temporary_matches)
    if(staging_temporary_owned)
      file(REMOVE "${staging_temporary_path}")
    endif()
    message(
      FATAL_ERROR
      "The temporary Stemgen model does not match the verified model: ${staging_temporary_path}"
    )
  endif()

  # Recheck the destination before the pathname-based rename. This closes all
  # cooperative-build changes observed by CMake. CMake 3.22 has no descriptor-
  # relative no-replace rename, so hostile replacement of the build directory
  # between this check and rename is outside this configure-time boundary.
  set(staged_path_has_symlink_component FALSE)
  _mixxx_stem_model_stage_path_has_symlink_component(
    "${staged_path}"
    staged_path_has_symlink_component
  )
  if(
    staged_path_has_symlink_component
    OR IS_SYMLINK "${staged_path}"
    OR IS_DIRECTORY "${staged_path}"
  )
    if(staging_temporary_owned)
      file(REMOVE "${staging_temporary_path}")
    endif()
    message(
      FATAL_ERROR
      "The staged Stemgen model destination changed before publication: ${staged_path}"
    )
  endif()

  file(
    RENAME
    "${staging_temporary_path}"
    "${staged_path}"
    RESULT staging_rename_result
  )
  if(NOT staging_rename_result STREQUAL "0")
    if(staging_temporary_owned)
      file(REMOVE "${staging_temporary_path}")
    endif()
    message(
      FATAL_ERROR
      "Could not publish the staged Stemgen model: ${staging_rename_result}"
    )
  endif()
  set(staging_temporary_owned FALSE)

  set(staged_model_matches FALSE)
  _mixxx_stem_model_stage_file_matches(
    "${staged_path}"
    "${expected_size}"
    "${expected_sha256}"
    staged_model_matches
  )
  if(NOT staged_model_matches)
    message(
      FATAL_ERROR
      "The published Stemgen model does not match the verified model: ${staged_path}"
    )
  endif()
endfunction()
