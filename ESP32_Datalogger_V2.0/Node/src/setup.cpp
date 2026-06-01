#include "../lib/can/frames.c"

bool use_oneshot = true;
void sensor_init(){
    // Maybe could do some sort of hashmap?
    
    #if defined(NODE_ID) && NODE_ID == 1
        use_oneshot = false;
        Sensor Front_brake;
        set_Sensor_id(&Front_brake,ID_Pa_FRONT_BRAKE);

        Sensor Rear_brake;
        set_Sensor_id(&Front_brake,ID_Pa_REAR_BRAKE);


    #elif defined(NODE_ID) && NODE_ID == 2
        use_oneshot = false;
        Sensor Engine_RPM;
        set_Sensor_id(&Front_brake,ID_RPM_ENGINE);

        Sensor Wheel_RPM;
        set_Sensor_id(&Front_brake,ID_RPM_WHEEL);

    #elif defined(NODE_ID) && NODE_ID == 3
        Sensor CVT;
        set_Sensor_id(&Front_brake,ID_Temp_CVT);

    #elif defined(NODE_ID) && NODE_ID == 4
        Sensor GPS;
        set_Sensor_id(&Front_brake,ID_GPS);        
    #elif defined(NODE_ID) && NODE_ID == 5
        #define SENSOR_MSG_ID ID_RPM_WHEEL
    #endif
}
