cmake_minimum_required(VERSION 3.16)
project(libsermctp LANGUAGES CXX)

# Set C++ standard for in-tree builds; the implementation's src/CMakeLists.txt
# also sets it but setting here keeps behavior consistent for top-level add_subdirectory.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Forward to the implementation directory which contains the full library build
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/src)
