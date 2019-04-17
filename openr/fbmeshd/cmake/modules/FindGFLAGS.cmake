# - Try to find GFLAGS
# Once done this will define
# GFLAGS_FOUND - System has GFLAGS
# GFLAGS_INCLUDE_DIRS - The GFLAGS include directories
# GFLAGS_LIBRARIES - The libraries needed to use GFLAGS
# GFLAGS_DEFINITIONS - Compiler switches required for using GFLAGS

find_path ( GFLAGS_INCLUDE_DIR gflags/gflags.h )
find_library ( GFLAGS_LIBRARY NAMES gflags )

set ( GFLAGS_LIBRARIES ${GFLAGS_LIBRARY} )
set ( GFLAGS_INCLUDE_DIRS ${GFLAGS_INCLUDE_DIR} )

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set GFLAGS_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( GFLAGS DEFAULT_MSG GFLAGS_LIBRARY GFLAGS_INCLUDE_DIR)
