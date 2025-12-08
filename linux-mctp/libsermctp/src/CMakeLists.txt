cmake_minimum_required(VERSION 3.16)
project(mctp-listener LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Library build options: build shared by default; static optional
option(BUILD_STATIC_LIBS "Also build a static archive of the library" OFF)

# Options
option(BUILD_TESTS "Build unit/integration tests" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer/UBSan (for CI/dev)" OFF)

if(ENABLE_ASAN)
  message(STATUS "Enabling ASAN/UBSAN flags")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Locate dependencies via pkg-config
find_package(PkgConfig REQUIRED)
# Require libnl development packages unconditionally. Fail configuration if
# libnl (libnl-3.0, libnl-genl-3.0, libnl-route-3.0) is not available.
pkg_check_modules(LIBNL3 REQUIRED libnl-3.0 libnl-genl-3.0 libnl-route-3.0)
include_directories(${LIBNL3_INCLUDE_DIRS})
link_directories(${LIBNL3_LIBRARY_DIRS})
add_definitions(${LIBNL3_CFLAGS_OTHER})

# libudev is required when ManagedUsbTty is used; link it unconditionally
pkg_check_modules(LIBUDEV REQUIRED libudev)
include_directories(${LIBUDEV_INCLUDE_DIRS})
link_directories(${LIBUDEV_LIBRARY_DIRS})
add_definitions(${LIBUDEV_CFLAGS_OTHER})

# Collect sources (explicit listing is preferred long-term)
file(GLOB_RECURSE MCTP_SRCS CONFIGURE_DEPENDS "*.cpp" "*.c")

# Ensure we don't accidentally include any test-only stubs which are no
# longer used; the build now requires libnl so `TestStubs.cpp` should not
# be present or compiled.

# Applications (such as `mctp-bridge`) are built and packaged separately
# from this library; the library build no longer needs to specially remove
# a top-level application source file from the source list.

# libnl is required; always build the real netlink implementation.
cmake_minimum_required(VERSION 3.16)
project(mctp-listener LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Library build options: build shared by default; static optional
option(BUILD_STATIC_LIBS "Also build a static archive of the library" OFF)

# Options
option(BUILD_TESTS "Build unit/integration tests" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer/UBSan (for CI/dev)" OFF)

if(ENABLE_ASAN)
  message(STATUS "Enabling ASAN/UBSAN flags")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Locate dependencies via pkg-config
find_package(PkgConfig REQUIRED)
# Require libnl development packages unconditionally. Fail configuration if
# libnl (libnl-3.0, libnl-genl-3.0, libnl-route-3.0) is not available.
pkg_check_modules(LIBNL3 REQUIRED libnl-3.0 libnl-genl-3.0 libnl-route-3.0)
include_directories(${LIBNL3_INCLUDE_DIRS})
link_directories(${LIBNL3_LIBRARY_DIRS})
add_definitions(${LIBNL3_CFLAGS_OTHER})

# Collect sources (explicit listing is preferred long-term)
file(GLOB_RECURSE MCTP_SRCS CONFIGURE_DEPENDS "*.cpp" "*.c")

# Ensure we don't accidentally include any test-only stubs which are no
# longer used; the build now requires libnl so `TestStubs.cpp` should not
# be present or compiled.

# Applications (such as `mctp-bridge`) are built and packaged separately
# from this library; the library build no longer needs to specially remove
# a top-level application source file from the source list.

# libnl is required; always build the real netlink implementation.
# No alternate stub implementation is supported anymore.

# Exclude SerialInspector from the library build when tests that exercise it
# are not desired. The file name contains two dots (SerialInspector..cpp) and
# is safe to remove from the source list.
list(REMOVE_ITEM MCTP_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/SerialInspector..cpp)

# We removed the NullModem helper; exclude its implementation from the library
list(REMOVE_ITEM MCTP_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/NullModem.cpp)

# Remove the VirtualSerial test helper from the library sources; this
# helper is provided as a standalone test utility and is not part of the
# library public implementation.
list(REMOVE_ITEM MCTP_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/VirtualSerial.cpp)

# Rename: exclude legacy MctpSerial.cpp (we now provide MctpFramer.cpp)
list(REMOVE_ITEM MCTP_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/MctpSerial.cpp)

# Ensure the MctpFramer implementation is included
list(FIND MCTP_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/MctpFramer.cpp" _found_mctp_framer)
if(_found_mctp_framer EQUAL -1)
  list(APPEND MCTP_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/MctpFramer.cpp)
endif()

# Build a static library with the core implementation so tests and the
# production executable can link the same objects.
## Build a shared library `sermctp` from the implementation sources.
add_library(sermctp SHARED ${MCTP_SRCS})
# Ensure the library can find public headers in ../include
target_include_directories(sermctp
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include/sermctp>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_compile_definitions(sermctp PRIVATE SERMCTP_BUILDING)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(NOT BUILD_TESTS)
    target_compile_options(sermctp PRIVATE -fvisibility=hidden -fPIC)
  else()
    target_compile_options(sermctp PRIVATE -fPIC)
  endif()
endif()

# Set library version and SONAME (SOVERSION) for stable ABI policy.
set_target_properties(sermctp PROPERTIES VERSION 1.0.0 SOVERSION 1)

if(BUILD_STATIC_LIBS)
  add_library(sermctp_static STATIC ${MCTP_SRCS})
  target_include_directories(sermctp_static
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  )
endif()
target_link_libraries(sermctp PUBLIC ${LIBNL3_LIBRARIES} ${LIBUDEV_LIBRARIES})

if(BUILD_TESTS)
  # When building tests in-tree, expose the tests/ headers to the build so
  # test-only helpers (e.g. Log.hpp) can be included by test helpers under src/.
  target_include_directories(sermctp PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../tests>)
endif()

# Production executable (small wrapper) links against the library.
# Only create the executable if the source file exists in this checkout.
## This directory builds the library implementation only. Application
## build rules (e.g. the former `mctp-bridge` executable) are intentionally
## excluded from the library packaging.

# Install
## Install library and headers
install(TARGETS sermctp
  EXPORT sermctpTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
)
if(BUILD_STATIC_LIBS)
  install(TARGETS sermctp_static ARCHIVE DESTINATION lib)
endif()

# Install public headers: only install the `sermctp` public API directory.
# Exclude the `detail/` implementation headers which are internal.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../include/sermctp DESTINATION include/sermctp
  PATTERN "detail" EXCLUDE
  PATTERN "*.md" EXCLUDE
)

## Export CMake target for downstream consumers
install(EXPORT sermctpTargets NAMESPACE sermctp:: DESTINATION lib/cmake/sermctp)

## Install a simple pkg-config file
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../sermctp.pc.in ${CMAKE_CURRENT_BINARY_DIR}/sermctp.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sermctp.pc DESTINATION lib/pkgconfig)

# Convenience custom target so developers can run: make install-app
add_custom_target(install-app
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR} --target install
  COMMENT "Build and install the library to the system (requires privileges)"
)

## Tests
if(BUILD_TESTS)
  enable_testing()
  # build test utilities under ../tests when requested
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()

