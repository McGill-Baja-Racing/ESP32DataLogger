# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/alex/.platformio/packages/framework-espidf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "/home/alex/.platformio/packages/framework-espidf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/.pio/build/esp32-p4/bootloader"
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/.pio/build/esp32-p4/bootloader-prefix"
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/.pio/build/esp32-p4/bootloader-prefix/tmp"
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/.pio/build/esp32-p4/bootloader-prefix/src/bootloader-stamp"
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/.pio/build/esp32-p4/bootloader-prefix/src"
  "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/.pio/build/esp32-p4/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/.pio/build/esp32-p4/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/alex/Coding/Baja/ESP32DataLogger/ESP32DataLogger/ESP32_Datalogger_V2.0/P4/masterp4/.pio/build/esp32-p4/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
