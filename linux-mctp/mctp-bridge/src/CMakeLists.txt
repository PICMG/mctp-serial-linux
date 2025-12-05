cmake_minimum_required(VERSION 3.16)
# This file is included by the parent mctp-bridge/CMakeLists.txt
# Parent should set  and C++ standard.

find_package(Threads REQUIRED)

# Create the executable
add_executable(mctp-bridge mctp-bridge.cpp)

# Prefer linking the exported namespaced target (installed via CMake),
# otherwise link the in-tree  target created by add_subdirectory.
if(TARGET sermctp::sermctp)
  target_link_libraries(mctp-bridge PRIVATE sermctp::sermctp Threads::Threads)
elseif(TARGET sermctp)
  target_link_libraries(mctp-bridge PRIVATE sermctp Threads::Threads)
else()
  message(FATAL_ERROR "No sermctp target available. Configure with USE_SYSTEM_SERMCTP=ON or ensure libsermctp is present in-tree.")
endif()

install(TARGETS mctp-bridge RUNTIME DESTINATION bin)
