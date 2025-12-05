cmake_minimum_required(VERSION 3.16)
# Builds the mctp-bridge executable but sources remain in ../src

find_package(Threads REQUIRED)

add_executable(mctp-bridge ../src/mctp-bridge.cpp)

if(TARGET sermctp::sermctp)
  target_link_libraries(mctp-bridge PRIVATE sermctp::sermctp Threads::Threads)
elseif(TARGET sermctp)
  target_link_libraries(mctp-bridge PRIVATE sermctp Threads::Threads)
else()
  message(FATAL_ERROR "No 'sermctp' target available. Configure with USE_SYSTEM_SERMCTP=ON or ensure libsermctp is present in-tree.")
endif()

install(TARGETS mctp-bridge RUNTIME DESTINATION bin)
