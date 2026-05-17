# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/remylaurendeau/esp/v5.5.1/esp-idf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "/Users/remylaurendeau/esp/v5.5.1/esp-idf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "/Users/remylaurendeau/Documents/Baja/BajaGit/ESP32DataLogger/ESP32_Datalogger_V2.0/Wireless/WIFI_Control_Prototype/build/bootloader"
  "/Users/remylaurendeau/Documents/Baja/BajaGit/ESP32DataLogger/ESP32_Datalogger_V2.0/Wireless/WIFI_Control_Prototype/build/bootloader-prefix"
  "/Users/remylaurendeau/Documents/Baja/BajaGit/ESP32DataLogger/ESP32_Datalogger_V2.0/Wireless/WIFI_Control_Prototype/build/bootloader-prefix/tmp"
  "/Users/remylaurendeau/Documents/Baja/BajaGit/ESP32DataLogger/ESP32_Datalogger_V2.0/Wireless/WIFI_Control_Prototype/build/bootloader-prefix/src/bootloader-stamp"
  "/Users/remylaurendeau/Documents/Baja/BajaGit/ESP32DataLogger/ESP32_Datalogger_V2.0/Wireless/WIFI_Control_Prototype/build/bootloader-prefix/src"
  "/Users/remylaurendeau/Documents/Baja/BajaGit/ESP32DataLogger/ESP32_Datalogger_V2.0/Wireless/WIFI_Control_Prototype/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/remylaurendeau/Documents/Baja/BajaGit/ESP32DataLogger/ESP32_Datalogger_V2.0/Wireless/WIFI_Control_Prototype/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/remylaurendeau/Documents/Baja/BajaGit/ESP32DataLogger/ESP32_Datalogger_V2.0/Wireless/WIFI_Control_Prototype/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
