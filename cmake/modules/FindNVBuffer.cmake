#.rst:
# FindNVBuffer
# --------
# Finds the NVidia Jetpack Multimedia SDK
#
# This will define the following variables::
#
# NVBUFFER_FOUND - system has NVBUFFER
# NVBUFFER_INCLUDE_DIRS - the NVBUFFER include directory
# NVBUFFER_LIBRARIES - the NVBUFFER libraries
# NVBUFFER_DEFINITIONS - the NVBUFFER compile definitions
#
# and the following imported targets::
#
#   NVBUFFER::NVBUFFER   - The NVBUFFER library

find_library(NVBUFFER_LIBRARY NAMES nvbuf_utils PATH_SUFFIXES tegra)

set(NVBUFFER_VERSION ${PC_NVBUFFER_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NVBUFFER
                                  REQUIRED_VARS NVBUFFER_LIBRARY)

if(NVBUFFER_FOUND)
  set(NVBUFFER_LIBRARIES ${NVBUFFER_LIBRARY})
  set(NVBUFFER_DEFINITIONS -DHAS_NVBUFFER=1)

  if(NOT TARGET NVBUFFER::NVBUFFER)
    add_library(NVBUFFER::NVBUFFER UNKNOWN IMPORTED)
    set_target_properties(NVBUFFER::NVBUFFER PROPERTIES
                                     IMPORTED_LOCATION "${NVBUFFER_LIBRARY}")
  endif()
endif()

mark_as_advanced(NVBUFFER_INCLUDE_DIR NVBUFFER_LIBRARY)
