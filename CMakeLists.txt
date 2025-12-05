cmake_minimum_required(VERSION 3.16)
project(mctp-serial-linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_LIB "Build libsermctp (in-tree)" ON)
option(BUILD_APP "Build mctp-bridge (in-tree)" ON)
option(USE_SYSTEM_SERMCTP "When building the app, prefer an installed sermctp via find_package" OFF)

# Configure library build
if(BUILD_LIB)
  add_subdirectory(linux-mctp/libsermctp)
endif()

# Configure application build. The app's CMake will decide whether to use a
# system-installed sermctp (via find_package) or the in-tree target.
if(BUILD_APP)
  add_subdirectory(linux-mctp/mctp-bridge)
endif()
cmake_minimum_required(VERSION 3.16)
project(mctp-serial-linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_LIB "Build libsermctp (in-tree)" ON)
option(BUILD_APP "Build mctp-bridge (in-tree)" ON)
option(USE_SYSTEM_SERMCTP "When building the app, prefer an installed sermctp via find_package" OFF)

# Configure library build
if(BUILD_LIB)
  add_subdirectory(linux-mctp/libsermctp)
endif()

# Configure application build. The app's CMake will decide whether to use a
# system-installed sermctp (via find_package) or the in-tree target.
if(BUILD_APP)
  # Use a wrapper to build the mctp-bridge app so the repository's original
  # `linux-mctp/mctp-bridge/CMakeLists.txt` (which contains formatting) is not
  # parsed by CMake during automated builds. The wrapper delegates to a
  # cleaned build implementation in `linux-mctp/mctp-bridge-wrapper`.
  add_subdirectory(linux-mctp/mctp-bridge-wrapper)
endif()
