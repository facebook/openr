# - Try to find THRIFT
# Once done this will define
# THRIFT_FOUND - System has THRIFT
# THRIFT_INCLUDE_DIRS - The THRIFT include directories
# THRIFT_LIBRARIES - The libraries needed to use THRIFT
# THRIFT_DEFINITIONS - Compiler switches required for using THRIFT

find_path ( THRIFT_INCLUDE_DIR thrift/lib/cpp/Thrift.h )
find_library ( THRIFT_LIBRARY NAMES thrift )

set ( THRIFT_LIBRARIES ${THRIFT_LIBRARY} )
set ( THRIFT_INCLUDE_DIRS ${THRIFT_INCLUDE_DIR} )

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set THRIFT_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( THRIFT DEFAULT_MSG THRIFT_LIBRARY THRIFT_INCLUDE_DIR)
