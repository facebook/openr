# - Try to find FOLLY
# Once done this will define
# FOLLY_FOUND - System has FOLLY
# FOLLY_INCLUDE_DIRS - The FOLLY include directories
# FOLLY_LIBRARIES - The libraries needed to use FOLLY
# FOLLY_DEFINITIONS - Compiler switches required for using FOLLY

find_path ( FOLLY_INCLUDE_DIR folly/Unicode.h)
find_library ( FOLLY_LIBRARY NAMES folly )

set ( FOLLY_LIBRARIES ${FOLLY_LIBRARY} )
set ( FOLLY_INCLUDE_DIRS ${FOLLY_INCLUDE_DIR} )

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set FOLLY_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( FOLLY DEFAULT_MSG FOLLY_LIBRARY FOLLY_INCLUDE_DIR)
