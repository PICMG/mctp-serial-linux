cmake_minimum_required(VERSION 3.16)
# This file is included by the parent mctp-bridge/CMakeLists.txt
# Ensure project and C++ standard are set by parent.

find_package(Threads REQUIRED)

# Add executable
add_executable(mctp-bridge mctp-bridge.cpp)

# Link against exported namespaced target if present (installed CMake package),
# otherwise link against the in-tree target created by add_subdirectory.
if(TARGET sermctp::sermctp)
  target_link_libraries(mctp-bridge PRIVATE sermctp::sermctp Threads::Threads)
elseif(TARGET sermctp)
  target_link_libraries(mctp-bridge PRIVATE sermctp Threads::Threads)
else()
  message(FATAL_ERROR "No 'sermctp' target available. Configure with USE_SYSTEM_SERMCTP=ON or ensure libsermctp is present in-tree.")
endif()

install(TARGETS mctp-bridge RUNTIME DESTINATION bin)
cmake_minimum_required(VERSION 3.16)
project(mctp-bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer linking the static library if available; fall back to shared.
if(NOT DEFINED SERMCTP_STATIC_PATH)
  set(SERMCTP_STATIC_PATH "${CMAKE_SOURCE_DIR}/../../libsermctp/build/libsermctp_static.a")
endif()
if(NOT DEFINED SERMCTP_SHARED_PATH)
  set(SERMCTP_SHARED_PATH "${CMAKE_SOURCE_DIR}/../../libsermctp/build/libsermctp.so")
endif()

add_executable(mctp-bridge mctp-bridge.cpp)
target_include_directories(mctp-bridge PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/../../libsermctp/include
  ${CMAKE_SOURCE_DIR}/../../libsermctp/include/sermctp
)

if(EXISTS ${SERMCTP_STATIC_PATH})
  message(STATUS "Linking mctp-bridge with static libsermctp: ${SERMCTP_STATIC_PATH}")
  target_link_libraries(mctp-bridge PRIVATE ${SERMCTP_STATIC_PATH} pthread)
else()
  if(EXISTS ${SERMCTP_SHARED_PATH})
    message(STATUS "Linking mctp-bridge with shared libsermctp: ${SERMCTP_SHARED_PATH}")
    target_link_libraries(mctp-bridge PRIVATE ${SERMCTP_SHARED_PATH} pthread)
    # embed rpath to find shared lib next to repo layout
    set_target_properties(mctp-bridge PROPERTIES INSTALL_RPATH "$ORIGIN/../libsermctp/build")
  else()
    message(FATAL_ERROR "libsermctp not found in expected build locations. Build libsermctp first.")
  endif()
endif()

install(TARGETS mctp-bridge RUNTIME DESTINATION bin)
