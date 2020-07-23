#.rst:
# FindV4L2
# --------
# Finds the Video For Linux library
#
# This will define the following variables::
#
# V4L2_FOUND - system has V4L2
# V4L2_INCLUDE_DIRS - the V4L2 include directory
# V4L2_LIBRARIES - the V4L2 libraries
# V4L2_DEFINITIONS - the V4L2 compile definitions
#
# and the following imported targets::
#
#   V4L2::V4L2   - The V4L2 library

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_V4L2 libv4l2 QUIET)
endif()

find_path(V4L2_INCLUDE_DIR NAMES libv4l2.h
                           PATHS ${PC_V4L2_INCLUDEDIR})
find_library(V4L2_LIBRARY NAMES v4l2 nvv4l2
                          PATHS ${PC_V4L2_LIBDIR}
                          PATH_SUFFIXES tegra)

set(V4L2_VERSION ${PC_V4L2_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(V4L2
                                  REQUIRED_VARS V4L2_LIBRARY V4L2_INCLUDE_DIR
                                  VERSION_VAR V4L2_VERSION)

if(V4L2_FOUND)
  set(V4L2_INCLUDE_DIRS "") # Don't want these added as 'timer.h' is a dangerous file
  set(V4L2_LIBRARIES ${V4L2_LIBRARY})
  set(V4L2_DEFINITIONS -DHAS_V4L2=1)

  if(NOT TARGET V4L2::V4L2)
    add_library(V4L2::V4L2 UNKNOWN IMPORTED)
    set_target_properties(V4L2::V4L2 PROPERTIES
                                     IMPORTED_LOCATION "${V4L2_LIBRARY}"
                                     INTERFACE_COMPILE_DEFINITIONS "${V4L2_DEFINITIONS}")
  endif()
endif()

mark_as_advanced(V4L2_INCLUDE_DIR V4L2_LIBRARY)
