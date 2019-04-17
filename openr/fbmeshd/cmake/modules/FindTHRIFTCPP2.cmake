# - Try to find THRIFTCPP2
# Once done this will define
# THRIFTCPP2_FOUND - System has THRIFTCPP2
# THRIFTCPP2_INCLUDE_DIRS - The THRIFTCPP2 include directories
# THRIFTCPP2_LIBRARIES - The libraries needed to use THRIFTCPP2
# THRIFTCPP2_DEFINITIONS - Compiler switches required for using THRIFTCPP2

find_path ( THRIFTCPP2_INCLUDE_DIR thrift/lib/cpp2/Thrift.h )
find_library ( THRIFTCPP2_LIBRARY NAMES thriftcpp2 )

set ( THRIFTCPP2_LIBRARIES ${THRIFTCPP2_LIBRARY} )
set ( THRIFTCPP2_INCLUDE_DIRS ${THRIFTCPP2_INCLUDE_DIR} )

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set THRIFTCPP2_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( THRIFTCPP2 DEFAULT_MSG THRIFTCPP2_LIBRARY THRIFTCPP2_INCLUDE_DIR)
