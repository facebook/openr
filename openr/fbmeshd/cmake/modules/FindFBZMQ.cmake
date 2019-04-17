# - Try to find FBZMQ
# Once done this will define
# FBZMQ_FOUND - System has FBZMQ
# FBZMQ_INCLUDE_DIRS - The FBZMQ include directories
# FBZMQ_LIBRARIES - The libraries needed to use FBZMQ
# FBZMQ_DEFINITIONS - Compiler switches required for using FBZMQ

find_path ( FBZMQ_INCLUDE_DIR fbzmq/zmq/Zmq.h async/ZmqEventLoop.h )
find_library ( FBZMQ_LIBRARY NAMES fbzmq )

set ( FBZMQ_LIBRARIES ${FBZMQ_LIBRARY} )
set ( FBZMQ_INCLUDE_DIRS ${FBZMQ_INCLUDE_DIR} )

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set FBZMQ_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( FBZMQ DEFAULT_MSG FBZMQ_LIBRARY FBZMQ_INCLUDE_DIR)
