#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "sermctp::sermctp" for configuration ""
set_property(TARGET sermctp::sermctp APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(sermctp::sermctp PROPERTIES
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libsermctp.so.1.0.0"
  IMPORTED_SONAME_NOCONFIG "libsermctp.so.1"
  )

list(APPEND _cmake_import_check_targets sermctp::sermctp )
list(APPEND _cmake_import_check_files_for_sermctp::sermctp "${_IMPORT_PREFIX}/lib/libsermctp.so.1.0.0" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
