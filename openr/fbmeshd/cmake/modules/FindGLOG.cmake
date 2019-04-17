# - Try to find GLOG
# Once done this will define
# GLOG_FOUND - System has GLOG
# GLOG_INCLUDE_DIRS - The GLOG include directories
# GLOG_LIBRARIES - The libraries needed to use GLOG
# GLOG_DEFINITIONS - Compiler switches required for using GLOG

find_path ( GLOG_INCLUDE_DIR glog/logging.h )
find_library ( GLOG_LIBRARY NAMES glog )

set ( GLOG_LIBRARIES ${GLOG_LIBRARY} )
set ( GLOG_INCLUDE_DIRS ${GLOG_INCLUDE_DIR} )

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set GLOG_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( GLOG DEFAULT_MSG GLOG_LIBRARY GLOG_INCLUDE_DIR)
