# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/alex/esp/v5.5.1/esp-idf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "/home/alex/esp/v5.5.1/esp-idf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/build/bootloader"
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/build/bootloader-prefix"
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/build/bootloader-prefix/tmp"
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/build/bootloader-prefix/src/bootloader-stamp"
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/build/bootloader-prefix/src"
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
