cmake_minimum_required(VERSION 3.2)
project(fbmeshd-if)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-noexcept-type")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-maybe-uninitialized")

find_library(PROTOCOL protocol PATHS)
find_library(TRANSPORT transport PATHS)
find_library(THRIFT thrift PATHS)
find_library(THRIFTCPP2 thriftcpp2 PATHS)
find_library(THRIFTPROTOCOL thriftprotocol PATHS)

# Include Thrift
find_program(THRIFT1 thrift1)
find_path(THRIFT_COMPILER_INCLUDE thrift/templates)
set(THRIFT_TEMPLATES ${THRIFT_COMPILER_INCLUDE}/thrift/templates)
include(${THRIFT_COMPILER_INCLUDE}/thrift/ThriftLibrary.cmake)

set(THRIFT_PREFIX "openr/fbmeshd/if")
set(THRIFT_DIR ${CMAKE_BINARY_DIR}/thrift-out/${THRIFT_PREFIX})
file(MAKE_DIRECTORY ${THRIFT_DIR})

include_directories(${CMAKE_BINARY_DIR}/thrift-out)

thrift_object(
  "fbmeshd" #file_name
  "MeshService" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

add_library(fbmeshd-if
  $<TARGET_OBJECTS:fbmeshd-cpp2-obj>
)

target_link_libraries(fbmeshd-if
  ${TRANSPORT}
  ${PROTOCOL}
  ${THRIFTPROTOCOL}
  ${THRIFTCPP2}
  ${THRIFT}
  -lpthread
)

install(TARGETS
  fbmeshd-if
  DESTINATION lib
)

# Install the generated thrift headers
install(DIRECTORY ${THRIFT_DIR}/gen-cpp2 DESTINATION include/${THRIFT_PREFIX}
        FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ${THRIFT_DIR}/gen-cpp2 DESTINATION include/${THRIFT_PREFIX}
        FILES_MATCHING PATTERN "*.tcc")
