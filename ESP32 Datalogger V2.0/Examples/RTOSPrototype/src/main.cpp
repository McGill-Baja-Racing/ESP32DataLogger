// #include <Arduino.h>
// #include <RTOS.h>
// #include <stdio.h>
// #include <FS.h>
// #include <esp_mac.h>

// using namespace std;
// // put function declarations here:
// int myFunction(int, int);

// const int SD_MMC_CMD = 15;
// const int SD_MMC_CLK = 14; 
// const int SD_MMC_D0 = 2;
// const int SD_MMC_D1 = 4;
// const int SD_MMC_D2 = 12;
// const int SD_MMC_D3 = 13;

// void setup() {
//   // put your setup code here, to run once:
//   int result = myFunction(2, 3);
//   Serial.begin(115200); // Initialize serial communication
//     string fileNameHead = "/abcd";
//     string fileNameEnd = ".csv";
//     int16_t fileNumber = 0;

//     const int8_t c_CoreZero = 0;
//     const int8_t c_CoreOne = 1; // Identify what arduino core uses, and pin it to the other one
    
//     TaskHandle_t *SDCardTask_handle;
//     TaskHandle_t *ReadSensors_handle;// do i need multiple ones?

//     xTaskCreatePinnedToCore(
//         WriteToSDCardTask, // the function to be tasked
//         "Writing to SD task", //Name
//         1000, //stack size, number of bytes allocated to it
//         NULL, // task parameters
//         1, // task priority The higher the number the less priority it has, starting from 0 (os is below that)
//         SDCardTask_handle, //task handle
//         c_CoreOne //core number where the task will be run
//     );
//     xTaskCreatePinnedToCore(
//         ReadSensors, // the function to be tasked
//         "Tick Read Sensor", //Name
//         1000, //stack size, number of bytes allocated to it
//         NULL, // task parameters
//         1, // task priority The higher the number the less priority it has, starting from 0 (os is below that)
//         ReadSensors_handle, //task handle
//         c_CoreZero //core number where the task will be run
//     );

//     Serial.println(xPortGetCoreID());
//     //SD_MMC.setPins(SD_MMC_CLK,SD_MMC_CMD,SD_MMC_D0);
//     // if (!SD_MMC.begin("/sdcard",true,true)) { 
//         // Serial.println("Failed to mount SD card"); 
//         // return;
//     // }
//     string num = to_string(fileNumber);
//     string fileName = fileNameHead + num + fileNameEnd;
//     //File file = SD_MMC.open(fileName.c_str() ,FILE_WRITE);
    


    
//     // We will use the ESP-IDF freeRTOS queing  ┌П┐(ಠ_ಠ)
//     // Main loop, Will become RTOSsed, so no loops
//     // while (true) {
//     //     file.print("hey \n");
//     //     delay(10);
//     //     file.flush();
//     // }
// }

// void loop() {
//   // put your main code here, to run repeatedly:
// }

// // put function definitions here:
// int myFunction(int x, int y) {
//   return x + y;
// }





// void WriteToSDCardTask(void *parameter){
//     vTaskDelay(1000/portTICK_PERIOD_MS); //suspends task for 1 sec (according to the tick period used)
// }
// void ReadSensors(void *parameter){

// }
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <vector>
#include <string>
#include <sstream>


namespace Config {
    constexpr int PIN_SD_CMD = 15;
    constexpr int PIN_SD_CLK = 14;
    constexpr int PIN_SD_D0  = 2;
    
    constexpr int PIN_SENSOR_1 = 34;
    constexpr int PIN_SENSOR_2 = 35;
    
    constexpr int QUEUE_SIZE = 50;
    constexpr int STACK_SIZE_READ = 4096;
    constexpr int STACK_SIZE_WRITE = 8192;
}


struct SensorData {
    uint32_t timestamp;
    int pin1Value;
    int pin2Value;
};


// Settings
const int QUEUE_LEN = 50;          // How many readings to buffer
const int READ_DELAY_MS = 10;      // Read every 10ms 
const std::string FILE_NAME = "/log.csv";


// Global Queue Handle
QueueHandle_t dataQueue;


// TASK 1: READ SENSORS (Core 0)
void TaskRead(void *pvParameters) {
    SensorData dataPacket;
    
    for (;;) {
        // 1. Acquire Data
        dataPacket.timestamp = millis();
        dataPacket.pin1Value = analogRead(Config::PIN_SENSOR_1);
        dataPacket.pin2Value = analogRead(Config::PIN_SENSOR_2);

        // 2. Push to Queue
        xQueueSend(dataQueue, &dataPacket, 0);

        // 3. Wait
        vTaskDelay(READ_DELAY_MS / portTICK_PERIOD_MS);
    }
}

// TASK 2: WRITE TO SD (Core 1)
void TaskWrite(void *pvParameters) {
    SensorData receivedData;
    char lineBuffer[64];

    
    for (;;) {
        // Wait indefinitely for new data
        if (xQueueReceive(dataQueue, &receivedData, portMAX_DELAY) == pdTRUE) {
            
            // 1. Format string 
            int len = snprintf(lineBuffer, sizeof(lineBuffer), "%lu,%d,%d\n", 
                receivedData.timestamp, 
                receivedData.pin1Value, 
                receivedData.pin2Value
            );

            // 2. Write to SD
            File file = SD_MMC.open(FILE_NAME.c_str(), FILE_APPEND);
            if (file) {
                file.write((uint8_t*)lineBuffer, len);
                file.close();
            } else {
                Serial.println("Write Failed");
            }
        }
    }
}



void setup() {
    Serial.begin(115200);

    // 1. Setup Pins
    pinMode(Config::PIN_SENSOR_1, INPUT);
    pinMode(Config::PIN_SENSOR_2, INPUT);

    // 2. Setup SD Card
    SD_MMC.setPins(Config::PIN_SD_CLK, Config::PIN_SD_CMD, Config::PIN_SD_D0);
    if (!SD_MMC.begin("/sdcard", true, true)) {
        Serial.println("SD Mount Failed");
        return;
    }
    
    // Write CSV Header
    File file = SD_MMC.open(FILE_NAME.c_str(), FILE_WRITE);
    if(file) {
        file.println("Timestamp,Sensor1,Sensor2");
        file.close();
    }

    // 3. Create Queue
    dataQueue = xQueueCreate(QUEUE_LEN, sizeof(SensorData));

    // 4. Launch Tasks
    // Rn both core 0
    xTaskCreatePinnedToCore(TaskRead, "Reader", 4096, NULL, 2, NULL,0);
    
    
    xTaskCreatePinnedToCore(TaskWrite, "Writer", 8192, NULL, 1, NULL, 0);
}

void loop() {
    vTaskDelete(NULL); // Remove loop task
}