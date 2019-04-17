# - Try to find SODIUM
# Once done this will define
# SODIUM_FOUND - System has SODIUM
# SODIUM_INCLUDE_DIRS - The SODIUM include directories
# SODIUM_LIBRARIES - The libraries needed to use SODIUM
# SODIUM_DEFINITIONS - Compiler switches required for using SODIUM

find_path ( SODIUM_INCLUDE_DIR sodium.h )
find_library ( SODIUM_LIBRARY NAMES sodium )

set ( SODIUM_LIBRARIES ${SODIUM_LIBRARY} )
set ( SODIUM_INCLUDE_DIRS ${SODIUM_INCLUDE_DIR} )

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set SODIUM_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( SODIUM DEFAULT_MSG SODIUM_LIBRARY SODIUM_INCLUDE_DIR)
