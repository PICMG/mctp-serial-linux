cmake_minimum_required(VERSION 3.16)
project(mctp-bridge-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# Virt serial test helper



add_executable(bridge-stress bridge_stress.cpp)
target_link_libraries(bridge-stress PRIVATE mctp_bridge_lib)
target_include_directories(bridge-stress PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Tests that were moved to `examples/` are not built here. Only build
# bridge tests if the sources are present in the tests/ directory.

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tty_to_linux_test.cpp")
	add_executable(tty-to-linux-test tty_to_linux_test.cpp)
	target_link_libraries(tty-to-linux-test PRIVATE mctp_bridge_lib)
	target_include_directories(tty-to-linux-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
	install(TARGETS tty-to-linux-test RUNTIME DESTINATION bin)
endif()


if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_linuxserial.cpp")
	add_executable(test_linuxserial test_linuxserial.cpp)
	target_link_libraries(test_linuxserial PRIVATE mctp_bridge_lib)
	target_include_directories(test_linuxserial PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
	install(TARGETS test_linuxserial RUNTIME DESTINATION bin)
endif()

# `nullmodem_test` removed intentionally; do not build it.


# Main tests runner
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_runner.cpp")
	add_executable(test-runner test_runner.cpp test_BcastMessenger.cpp test_MctpFramer_decode.cpp)
	target_link_libraries(test-runner PRIVATE mctp_bridge_lib)
	target_include_directories(test-runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
	target_compile_definitions(test-runner PRIVATE "MCTP_RANDOM_JSON_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/mctp_random_messages.json\"")
	install(TARGETS test-runner RUNTIME DESTINATION bin)
endif()
