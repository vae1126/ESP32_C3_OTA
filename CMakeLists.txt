# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mqtt_ha)

# Firmware version for OTA update checking
# Increment this before each release
set(FIRMWARE_VERSION "3.0.0")
add_compile_definitions(FIRMWARE_VERSION="${FIRMWARE_VERSION}")
