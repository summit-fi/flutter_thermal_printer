cmake_minimum_required(VERSION 3.10)

set(PROJECT_NAME "flutter_thermal_printer")
project(${PROJECT_NAME} LANGUAGES CXX)

cmake_policy(SET CMP0063 NEW)

set(PLUGIN_NAME "flutter_thermal_printer_plugin")

# Find system dependencies.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)

# Find CUPS via find_library/find_path to avoid transitive pkg-config issues
# (gnutls, avahi-client may not have .pc files on all systems).
find_library(CUPS_LIBRARY NAMES cups REQUIRED)
find_path(CUPS_INCLUDE_DIR NAMES cups/cups.h REQUIRED)
find_library(BLUETOOTH_LIBRARY NAMES bluetooth REQUIRED)

list(APPEND PLUGIN_SOURCES
  "flutter_thermal_printer_plugin.cc"
)

add_library(${PLUGIN_NAME} SHARED
  ${PLUGIN_SOURCES}
)

apply_standard_settings(${PLUGIN_NAME})

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)

target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_include_directories(${PLUGIN_NAME} PRIVATE
  ${CUPS_INCLUDE_DIR})

target_link_libraries(${PLUGIN_NAME} PRIVATE
  flutter
  PkgConfig::GTK
  ${CUPS_LIBRARY}
  ${BLUETOOTH_LIBRARY})
