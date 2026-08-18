#[=======================================================================[.rst:
FindBungee
----------

Finds the Bungee audio stretcher library.

This module is invoked via ``find_package(Bungee MODULE)``.  Before reaching
this module the caller should already have tried CONFIG-mode packages:

.. code-block:: cmake

  find_package(Bungee CONFIG QUIET)         # upstream config package
  find_package(unofficial-bungee CONFIG QUIET) # vcpkg PR #50120

This module handles the remaining discovery paths:

1. ``pkg-config`` module ``libbungee`` (installed by Bungee or its vcpkg port).
2. Manual ``find_path`` / ``find_library`` search (system installs,
  developer build directories).

Imported Targets
^^^^^^^^^^^^^^^^

This module provides the following imported targets, if found:

``Bungee::Bungee``
  The Bungee library

Result Variables
^^^^^^^^^^^^^^^^

This will define the following variables:

``Bungee_FOUND``
  True if the system has the Bungee library.
``Bungee_INCLUDE_DIRS``
  Include directories needed to use Bungee.
``Bungee_LIBRARIES``
  Libraries needed to link to Bungee.

Cache Variables
^^^^^^^^^^^^^^^

The following cache variables may also be set:

``Bungee_INCLUDE_DIR``
  The directory containing ``bungee/Bungee.h``.
``Bungee_LIBRARY``
  The path to the Bungee library.

#]=======================================================================]

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_Bungee QUIET libbungee)
endif()

find_path(
  Bungee_INCLUDE_DIR
  NAMES bungee/Bungee.h
  HINTS
    ${PC_Bungee_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../bungee
    ${CMAKE_SOURCE_DIR}/../bungee
  DOC "Bungee include directory"
)
mark_as_advanced(Bungee_INCLUDE_DIR)

find_library(
  Bungee_LIBRARY
  NAMES bungee libbungee
  HINTS
    ${PC_Bungee_LIBRARY_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../bungee/build
    ${CMAKE_SOURCE_DIR}/../bungee/build
  DOC "Bungee library"
)
mark_as_advanced(Bungee_LIBRARY)

if(DEFINED PC_Bungee_VERSION AND NOT PC_Bungee_VERSION STREQUAL "")
  set(Bungee_VERSION "${PC_Bungee_VERSION}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Bungee
  REQUIRED_VARS Bungee_LIBRARY Bungee_INCLUDE_DIR
  VERSION_VAR Bungee_VERSION
)

if(Bungee_FOUND)
  set(Bungee_LIBRARIES "${Bungee_LIBRARY}")
  set(Bungee_INCLUDE_DIRS "${Bungee_INCLUDE_DIR}")

  if(NOT TARGET Bungee::Bungee)
    set(_Bungee_INTERFACE_LINK_LIBRARIES)
    set(_Bungee_INTERFACE_LINK_DIRECTORIES)
    set(_Bungee_INTERFACE_LINK_OPTIONS)
    if(PC_Bungee_FOUND)
      set(_Bungee_PC_LINK_LIBRARIES ${PC_Bungee_LINK_LIBRARIES})
      set(_Bungee_PC_LIBRARY_DIRS ${PC_Bungee_LIBRARY_DIRS})
      set(_Bungee_PC_LDFLAGS_OTHER ${PC_Bungee_LDFLAGS_OTHER})
      if(Bungee_LIBRARY MATCHES "\\${CMAKE_STATIC_LIBRARY_SUFFIX}$")
        if(PC_Bungee_STATIC_LINK_LIBRARIES)
          set(_Bungee_PC_LINK_LIBRARIES ${PC_Bungee_STATIC_LINK_LIBRARIES})
        endif()
        if(PC_Bungee_STATIC_LIBRARY_DIRS)
          set(_Bungee_PC_LIBRARY_DIRS ${PC_Bungee_STATIC_LIBRARY_DIRS})
        endif()
        if(PC_Bungee_STATIC_LDFLAGS_OTHER)
          set(_Bungee_PC_LDFLAGS_OTHER ${PC_Bungee_STATIC_LDFLAGS_OTHER})
        endif()
      endif()

      get_filename_component(_Bungee_LIBRARY_NAME "${Bungee_LIBRARY}" NAME_WE)
      string(
        REGEX REPLACE
        "^lib"
        ""
        _Bungee_LIBRARY_NAME
        "${_Bungee_LIBRARY_NAME}"
      )
      foreach(_Bungee_PC_LINK_LIBRARY IN LISTS _Bungee_PC_LINK_LIBRARIES)
        get_filename_component(
          _Bungee_PC_LINK_LIBRARY_NAME
          "${_Bungee_PC_LINK_LIBRARY}"
          NAME_WE
        )
        string(
          REGEX REPLACE
          "^-l|^lib"
          ""
          _Bungee_PC_LINK_LIBRARY_NAME
          "${_Bungee_PC_LINK_LIBRARY_NAME}"
        )
        if(
          NOT _Bungee_PC_LINK_LIBRARY STREQUAL Bungee_LIBRARY
          AND NOT _Bungee_PC_LINK_LIBRARY_NAME STREQUAL _Bungee_LIBRARY_NAME
        )
          list(
            APPEND
            _Bungee_INTERFACE_LINK_LIBRARIES
            "${_Bungee_PC_LINK_LIBRARY}"
          )
        endif()
      endforeach()
      set(_Bungee_INTERFACE_LINK_DIRECTORIES ${_Bungee_PC_LIBRARY_DIRS})
      set(_Bungee_INTERFACE_LINK_OPTIONS ${_Bungee_PC_LDFLAGS_OTHER})
    endif()

    add_library(Bungee::Bungee UNKNOWN IMPORTED)
    set_target_properties(
      Bungee::Bungee
      PROPERTIES
        IMPORTED_LOCATION "${Bungee_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Bungee_INCLUDE_DIR}"
    )
    if(_Bungee_INTERFACE_LINK_LIBRARIES)
      set_property(
        TARGET Bungee::Bungee
        PROPERTY INTERFACE_LINK_LIBRARIES "${_Bungee_INTERFACE_LINK_LIBRARIES}"
      )
    endif()
    if(_Bungee_INTERFACE_LINK_DIRECTORIES)
      set_property(
        TARGET Bungee::Bungee
        PROPERTY
          INTERFACE_LINK_DIRECTORIES "${_Bungee_INTERFACE_LINK_DIRECTORIES}"
      )
    endif()
    if(_Bungee_INTERFACE_LINK_OPTIONS)
      set_property(
        TARGET Bungee::Bungee
        PROPERTY INTERFACE_LINK_OPTIONS "${_Bungee_INTERFACE_LINK_OPTIONS}"
      )
    endif()
    unset(_Bungee_INTERFACE_LINK_LIBRARIES)
    unset(_Bungee_INTERFACE_LINK_DIRECTORIES)
    unset(_Bungee_INTERFACE_LINK_OPTIONS)
    unset(_Bungee_LIBRARY_NAME)
    unset(_Bungee_PC_LDFLAGS_OTHER)
    unset(_Bungee_PC_LIBRARY_DIRS)
    unset(_Bungee_PC_LINK_LIBRARIES)
    unset(_Bungee_PC_LINK_LIBRARY)
    unset(_Bungee_PC_LINK_LIBRARY_NAME)
  endif()
endif()
