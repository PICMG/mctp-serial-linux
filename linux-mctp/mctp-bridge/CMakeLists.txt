cmake_minimum_required(VERSION 3.16)
project(mctp-bridge LANGUAGES CXX)
cmake_minimum_required(VERSION 3.16)
project(mctp-bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_SYSTEM_SERMCTP "Prefer an installed sermctp (find_package) instead of in-tree build" OFF)
option(FALLBACK_TO_IN_TREE_SERMCTP "If find_package fails, add_subdirectory(linux-mctp/libsermctp)" ON)

if(USE_SYSTEM_SERMCTP)
  find_package(sermctp CONFIG QUIET)
endif()

# If we don't have a sermctp target, optionally add the in-tree libsermctp.
if(NOT TARGET sermctp AND NOT TARGET sermctp::sermctp)
  if(FALLBACK_TO_IN_TREE_SERMCTP)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libsermctp ${CMAKE_BINARY_DIR}/libsermctp-build)
  endif()
endif()

# Now build the app's sources (keeps src/ focused on the executable itself)
# Use src2 which builds the executable but reads the implementation from ../src
add_subdirectory(src2)
