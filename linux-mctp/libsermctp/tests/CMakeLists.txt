cmake_minimum_required(VERSION 3.16)
project(libsermctp-tests LANGUAGES CXX)

# Gather test sources
file(GLOB TEST_SRCS CONFIGURE_DEPENDS "*.cpp")

# Provide JSON fixture to tests if present
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/mctp_random_messages.json)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/mctp_random_messages.json ${CMAKE_CURRENT_BINARY_DIR}/mctp_random_messages.json COPYONLY)
endif()

# Exclude examples/legacy/non-unit sources (no-op if absent)
list(REMOVE_ITEM TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/virt_serial.cpp)
list(REMOVE_ITEM TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/nullmodem_test.cpp)
list(REMOVE_ITEM TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/test_endpoint.cpp)
list(REMOVE_ITEM TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/linux_to_tty_test.cpp)
list(REMOVE_ITEM TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/tty_to_linux_test.cpp)
list(REMOVE_ITEM TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/bridge_stress_main.cpp)

# Split into aggregated (no main) and standalone (have main)
set(AGG_SRCS)
set(STANDALONE_SRCS)
foreach(_src IN LISTS TEST_SRCS)
  get_filename_component(_name ${_src} NAME)
  if(_name STREQUAL "test_runner.cpp")
    list(APPEND AGG_SRCS ${_src})
    continue()
  endif()
  file(READ ${_src} _content)
  string(FIND "${_content}" "int main(" _pos)
  if(_pos EQUAL -1)
    list(APPEND AGG_SRCS ${_src})
  else()
    list(APPEND STANDALONE_SRCS ${_src})
  endif()
endforeach()

# Aggregated runner
add_executable(test_runner ${AGG_SRCS})
target_link_libraries(test_runner PRIVATE sermctp)
target_include_directories(test_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

set(MCTP_JSON ${CMAKE_CURRENT_SOURCE_DIR}/mctp_random_messages.json)
if(EXISTS ${MCTP_JSON})
  add_custom_command(TARGET test_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MCTP_JSON} $<TARGET_FILE_DIR:test_runner>/mctp_random_messages.json
  )
endif()

# Standalone executables
foreach(_exe_src IN LISTS STANDALONE_SRCS)
  get_filename_component(_exe_name ${_exe_src} NAME_WE)
  add_executable(${_exe_name} ${_exe_src})
  target_link_libraries(${_exe_name} PRIVATE sermctp)
  target_include_directories(${_exe_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )
  if(EXISTS ${MCTP_JSON})
    add_custom_command(TARGET ${_exe_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MCTP_JSON} $<TARGET_FILE_DIR:${_exe_name}>/mctp_random_messages.json
    )
  endif()
  add_test(NAME ${_exe_name} COMMAND ${_exe_name})
  if("${_exe_name}" STREQUAL "test_linuxserial")
    set_tests_properties(${_exe_name} PROPERTIES LABELS "integration;requires_root")
  endif()
endforeach()

find_package(Threads QUIET)
if(THREADS_FOUND)
  target_link_libraries(test_runner PRIVATE Threads::Threads)
endif()

# Aggregated runner contains integration/stress tests; mark accordingly
add_test(NAME libsermctp-tests COMMAND test_runner)
set_tests_properties(libsermctp-tests PROPERTIES LABELS "integration;requires_root")
