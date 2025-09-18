
// ESP32 datalogging code
// Author: Remy Laurendeau
// Date: 30 May 2025

// This is our basic code for logging data with our ESP32. The details about the connections are bellow.


//  Remy Notes:
  // - Connect RX of the GPS to TX idicated here and same for TX of the GPS to RX 
  // - There will only be data displayed if there is a red light flashing on GPS board


// SD card imports and data pins
#include "sd_read_write.h"
#include "SD_MMC.h"
#include "FS.h"

#define SD_MMC_CMD 15 //Please do not modify it.
#define SD_MMC_CLK 14 //Please do not modify it. 
#define SD_MMC_D0  2  //Please do not modify it.

// GPS imports and data pins
#include <TinyGPS++.h>

// Define the RX and TX pins for Serial 2
#define RXD2 19
#define TXD2 18

#define GPS_BAUD 9600

// The TinyGPS++ object
TinyGPSPlus gps;

// Create an instance of the HardwareSerial class for Serial 2
HardwareSerial gpsSerial(2);

#include <SPI.h>
File logFile;
int fileNumber = 0;
char filename[] = "/data000.csv";
unsigned long newFileMillis; // time since last file created

void setup() {
  Serial.begin(115200);

  // SD setup
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
  if (!SD_MMC.begin("/sdcard", true, true, SDMMC_FREQ_DEFAULT, 5)) {
    Serial.println("Card Mount Failed");
    return;
  }

  uint8_t cardType = SD_MMC.cardType();
  if(cardType == CARD_NONE){
      Serial.println("No SD_MMC card attached");
      return;
  }

  //writeFile(SD_MMC, "/data.csv", "Time stamp, Speed, X accel, Y accel, Z accel, Engin RPM, Wheel RPM\n ");



  // Start GPS Serial with the defined RX and TX pins and a baud rate of 9600
  //gpsSerial.begin(GPS_BAUD, SERIAL_8N1, RXD2, TXD2);
  //Serial.println("Serial 2 started at 9600 baud rate");

  openNewFile();

}

void loop() {
  String data = "";

  // Timestamp
  unsigned long timestamp = millis();
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";
  data = data + timestamp + ", ";


  // GPS
  //while (gpsSerial.available() > 0) {
  //  gps.encode(gpsSerial.read());
  //}
  //data = data + gps.speed.kmph() + ", ";

  // Writting to file
  data = data + "\n";

  if(millis() - newFileMillis > 15000) {
    logFile.close();
    openNewFile();
    newFileMillis = millis();
  }
  // File was opened and switch is on
  // Serial.println(String(switchState));
  logFile.print(data);
  //writeFile(SD_MMC, "/data.csv", "Time stamp, Speed, X accel, Y accel, Z accel, Engin RPM, Wheel RPM\n ");

  // Sample every 1 ms
  delay(1);
}

void openNewFile() {
// closing old file
  //logFile.close();

  fileNumber++;
  filename[5] = fileNumber / 100 + '0';
  filename[6] = fileNumber % 100 / 10 + '0';
  filename[7] = fileNumber % 10 + '0';
  Serial.println(filename);
  logFile = SD_MMC.open(filename, FILE_WRITE);
  logFile.println(F("Time (ms),Acceleration X (1/16384 g),Y,Z,Driveshaft Output Voltage,Sparkplug Voltage,Speed (km/h),Longitude,Latitude,Altitude,Pressure 1 (PSI),Pressure 1 (PSI),IR"));
  //"Time (ms),Yaw (deg),Pitch (deg),Roll (deg),Acceleration X (1/16384 g),Y,Z,Angular Velocity X (deg/s),Y,Z,Driveshaft Output Voltage,Sparkplug Voltage"
}
