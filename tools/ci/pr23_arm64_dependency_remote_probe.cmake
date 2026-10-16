cmake_minimum_required(VERSION 3.30)

if(NOT DEFINED PROBE_DIR OR NOT DEFINED RESULT_FILE)
  message(FATAL_ERROR "PROBE_DIR and RESULT_FILE are required")
endif()

file(MAKE_DIRECTORY "${PROBE_DIR}")

set(_expected_url
    "https://downloads.mixxx.org/dependencies/2.7-rel/Windows/mixxx-deps-2.7-arm64-windows-rel-ee6dc7fe.zip"
)
set(_expected_sha256
    "38218f4874d3852a0689009385fe0106ff089511e37d7c062bc698449c8b9110"
)
set(_source_pin_precondition_ok TRUE)
if(NOT "$ENV{BUILDENV_URL}" STREQUAL "${_expected_url}"
   OR NOT "$ENV{BUILDENV_SHA256}" STREQUAL "${_expected_sha256}"
)
  set(_source_pin_precondition_ok FALSE)
endif()

set(_download_attempted FALSE)
set(_download_status_code "")
set(_download_status_message "")
set(_transfer_log "")
set(_archive_path "${PROBE_DIR}/mixxx-deps-2.7-arm64-windows-rel-ee6dc7fe.zip")
file(REMOVE "${_archive_path}")

if(_source_pin_precondition_ok)
  set(_download_attempted TRUE)
  # Match CMakeLists.txt's file(DOWNLOAD) GET and TLS behavior. STATUS and LOG
  # expose transfer details; the timeouts bound this one remote archive fetch.
  file(
    DOWNLOAD "${_expected_url}" "${_archive_path}"
    SHOW_PROGRESS
    TLS_VERIFY ON
    NETRC IGNORED
    TIMEOUT 300
    INACTIVITY_TIMEOUT 60
    STATUS _download_status
    LOG _transfer_log
  )
  list(GET _download_status 0 _download_status_code)
  list(GET _download_status 1 _download_status_message)
endif()

# CMake's LOG contains libcurl's verbose request/response trace. Keep it in
# memory and retain only the last response status plus a strict header allowlist.
set(_response_status_code "")
set(_content_length "")
set(_content_type "")
set(_last_modified "")
set(_etag "")
set(_content_encoding "")
set(_content_range "")
set(_accept_ranges "")
set(_in_response_headers FALSE)
set(_pending_connect FALSE)
set(_remaining_log "${_transfer_log}")
while(NOT "${_remaining_log}" STREQUAL "")
  string(FIND "${_remaining_log}" "\n" _newline_index)
  if(_newline_index LESS 0)
    set(_line "${_remaining_log}")
    set(_remaining_log "")
  else()
    string(SUBSTRING "${_remaining_log}" 0 ${_newline_index} _line)
    math(EXPR _next_index "${_newline_index} + 1")
    string(SUBSTRING "${_remaining_log}" ${_next_index} -1 _remaining_log)
  endif()
  string(REPLACE "\r" "" _line "${_line}")
  string(STRIP "${_line}" _line)
  # CMake 3.30's curl debug callback stores HTTP headers verbatim. Accept the
  # customary curl verbose prefix as well, without retaining any raw log text.
  string(REGEX REPLACE "^[<>] +" "" _line "${_line}")

  if(_line MATCHES "^CONNECT [^ ]+ HTTP/[0-9.]+")
    set(_pending_connect TRUE)
    set(_in_response_headers FALSE)
  elseif(_line MATCHES "^HTTP/[0-9.]+[ \t]+([0-9][0-9][0-9])")
    if(_pending_connect)
      # Ignore an HTTPS proxy's CONNECT response; it is not the asset response.
      set(_pending_connect FALSE)
      set(_in_response_headers FALSE)
    else()
      set(_response_status_code "${CMAKE_MATCH_1}")
      set(_content_length "")
      set(_content_type "")
      set(_last_modified "")
      set(_etag "")
      set(_content_encoding "")
      set(_content_range "")
      set(_accept_ranges "")
      set(_in_response_headers TRUE)
    endif()
  elseif(_in_response_headers)
    if(_line STREQUAL "")
      set(_in_response_headers FALSE)
    elseif(_line MATCHES "^[Cc]ontent-[Ll]ength:[ \t]*([0-9]+)$")
      set(_content_length "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^[Cc]ontent-[Tt]ype:[ \t]*(.*)$")
      set(_content_type "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^[Ll]ast-[Mm]odified:[ \t]*(.*)$")
      set(_last_modified "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^[Ee][Tt]ag:[ \t]*(.*)$")
      set(_etag "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^[Cc]ontent-[Ee]ncoding:[ \t]*(.*)$")
      set(_content_encoding "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^[Cc]ontent-[Rr]ange:[ \t]*(.*)$")
      set(_content_range "${CMAKE_MATCH_1}")
    elseif(_line MATCHES "^[Aa]ccept-[Rr]anges:[ \t]*(.*)$")
      set(_accept_ranges "${CMAKE_MATCH_1}")
    endif()
  endif()
endwhile()

set(_archive_present FALSE)
set(_archive_bytes "")
set(_actual_sha256 "")
set(_prefix_hex "")
set(_zip_magic_hex "")
set(_zip_magic_kind "")
set(_body_classification "no_body")
if(EXISTS "${_archive_path}")
  set(_archive_present TRUE)
  file(SIZE "${_archive_path}" _archive_bytes)
  file(SHA256 "${_archive_path}" _actual_sha256)
  if(_archive_bytes GREATER 0)
    file(READ "${_archive_path}" _prefix_hex LIMIT 64 HEX)
    string(TOLOWER "${_prefix_hex}" _normalized_prefix)
    string(REGEX REPLACE "^(20|09|0a|0d)+" "" _normalized_prefix
                         "${_normalized_prefix}")
    string(LENGTH "${_prefix_hex}" _prefix_length)
    if(_prefix_length GREATER_EQUAL 8)
      string(SUBSTRING "${_prefix_hex}" 0 8 _zip_magic_hex)
    else()
      set(_zip_magic_hex "${_prefix_hex}")
    endif()

    if(_normalized_prefix MATCHES "^504b0304")
      set(_zip_magic_kind "zip_local_file_header")
      set(_body_classification "zip")
    elseif(_normalized_prefix MATCHES "^504b0506")
      set(_zip_magic_kind "zip_end_of_central_directory")
      set(_body_classification "zip")
    elseif(_normalized_prefix MATCHES "^504b0708")
      set(_zip_magic_kind "zip_data_descriptor")
      set(_body_classification "zip")
    elseif(_normalized_prefix MATCHES
           "^(efbbbf)?(3c21646f6374797065|3c21444f4354595045)"
           OR _normalized_prefix MATCHES
           "^(efbbbf)?(3c68746d6c|3c48544d4c)")
      set(_body_classification "html")
    elseif(_normalized_prefix MATCHES "^(efbbbf)?(3c3f786d6c|3c3f584d4c)")
      set(_body_classification "xml")
    elseif(_normalized_prefix MATCHES "^(efbbbf)?(7b|5b)")
      set(_body_classification "json_like")
    else()
      set(_body_classification "other_non_zip")
    endif()
  else()
    set(_body_classification "empty")
  endif()
endif()

function(_json_quote _input _output)
  set(_value "${_input}")
  string(REPLACE "\\" "\\\\" _value "${_value}")
  string(REPLACE "\"" "\\\"" _value "${_value}")
  string(REPLACE "\r" "\\r" _value "${_value}")
  string(REPLACE "\n" "\\n" _value "${_value}")
  string(REPLACE "\t" "\\t" _value "${_value}")
  set(${_output} "\"${_value}\"" PARENT_SCOPE)
endfunction()

function(_json_nullable_string _input _output)
  if("${_input}" STREQUAL "")
    set(${_output} null PARENT_SCOPE)
  else()
    string(LENGTH "${_input}" _length)
    if(_length GREATER 256 OR "${_input}" MATCHES "https?://|[?#]")
      set(${_output} null PARENT_SCOPE)
    else()
      _json_quote("${_input}" _quoted)
      set(${_output} "${_quoted}" PARENT_SCOPE)
    endif()
  endif()
endfunction()

if(_download_attempted)
  set(_download_attempted_json true)
  set(_download_status_code_json "${_download_status_code}")
  _json_quote("${_download_status_message}" _download_status_message_json)
else()
  set(_download_attempted_json false)
  set(_download_status_code_json null)
  set(_download_status_message_json null)
endif()

if(_response_status_code STREQUAL "")
  set(_response_status_code_json null)
else()
  set(_response_status_code_json "${_response_status_code}")
endif()

if(_content_length STREQUAL "")
  set(_content_length_json null)
else()
  set(_content_length_json "${_content_length}")
endif()

_json_nullable_string("${_content_type}" _content_type_json)
_json_nullable_string("${_last_modified}" _last_modified_json)
_json_nullable_string("${_etag}" _etag_json)
_json_nullable_string("${_content_encoding}" _content_encoding_json)
_json_nullable_string("${_content_range}" _content_range_json)
_json_nullable_string("${_accept_ranges}" _accept_ranges_json)

if(_archive_present)
  set(_archive_present_json true)
  set(_archive_bytes_json "${_archive_bytes}")
  _json_quote("${_actual_sha256}" _actual_sha256_json)
  _json_quote("${_prefix_hex}" _prefix_hex_json)
  _json_quote("${_zip_magic_hex}" _zip_magic_hex_json)
  _json_quote("${_zip_magic_kind}" _zip_magic_kind_json)
  _json_quote("${_body_classification}" _body_classification_json)
  if(_actual_sha256 STREQUAL "${_expected_sha256}")
    set(_expected_sha256_match_json true)
  else()
    set(_expected_sha256_match_json false)
  endif()
else()
  set(_archive_present_json false)
  set(_archive_bytes_json null)
  set(_actual_sha256_json null)
  set(_prefix_hex_json null)
  set(_zip_magic_hex_json null)
  set(_zip_magic_kind_json null)
  _json_quote("${_body_classification}" _body_classification_json)
  set(_expected_sha256_match_json null)
endif()

if(_download_attempted AND _download_status_code STREQUAL "0")
  set(_download_completed_json true)
else()
  set(_download_completed_json false)
endif()

if(_source_pin_precondition_ok)
  set(_source_pin_precondition_ok_json true)
  set(_diagnostic_status "completed")
else()
  set(_source_pin_precondition_ok_json false)
  set(_diagnostic_status "skipped_source_pin_mismatch")
endif()

string(TIMESTAMP _captured_at "%Y-%m-%dT%H:%M:%SZ" UTC)
_json_quote("${_expected_url}" _expected_url_json)
_json_quote("${_expected_sha256}" _expected_sha256_json)
_json_quote("${_diagnostic_status}" _diagnostic_status_json)
_json_quote("${_captured_at}" _captured_at_json)

# Delete the downloaded archive before writing the result artifact.
file(REMOVE "${_archive_path}")
file(
  WRITE "${RESULT_FILE}"
  "{\n"
  "  \"schema_version\": 1,\n"
  "  \"diagnostic_status\": ${_diagnostic_status_json},\n"
  "  \"diagnostic_completed\": true,\n"
  "  \"captured_at_utc\": ${_captured_at_json},\n"
  "  \"source_pin_precondition_ok\": ${_source_pin_precondition_ok_json},\n"
  "  \"requested_url\": ${_expected_url_json},\n"
  "  \"expected_sha256\": ${_expected_sha256_json},\n"
  "  \"download_attempted\": ${_download_attempted_json},\n"
  "  \"download_status_code\": ${_download_status_code_json},\n"
  "  \"download_status_message\": ${_download_status_message_json},\n"
  "  \"download_completed\": ${_download_completed_json},\n"
  "  \"http_response_status_code\": ${_response_status_code_json},\n"
  "  \"response_headers\": {\n"
  "    \"content_length\": ${_content_length_json},\n"
  "    \"content_type\": ${_content_type_json},\n"
  "    \"last_modified\": ${_last_modified_json},\n"
  "    \"etag\": ${_etag_json},\n"
  "    \"content_encoding\": ${_content_encoding_json},\n"
  "    \"content_range\": ${_content_range_json},\n"
  "    \"accept_ranges\": ${_accept_ranges_json}\n"
  "  },\n"
  "  \"archive_file_present\": ${_archive_present_json},\n"
  "  \"archive_bytes_received\": ${_archive_bytes_json},\n"
  "  \"actual_sha256\": ${_actual_sha256_json},\n"
  "  \"expected_sha256_matches\": ${_expected_sha256_match_json},\n"
  "  \"first_zip_magic_hex\": ${_zip_magic_hex_json},\n"
  "  \"zip_magic_kind\": ${_zip_magic_kind_json},\n"
  "  \"body_classification\": ${_body_classification_json},\n"
  "  \"body_prefix_hex_max_64_bytes\": ${_prefix_hex_json},\n"
  "  \"raw_transfer_log_saved\": false,\n"
  "  \"dependency_archive_removed\": true,\n"
  "  \"dependency_archive_uploaded\": false\n"
  "}\n"
)

message(STATUS "Dependency byte diagnostic JSON written; archive removed")
