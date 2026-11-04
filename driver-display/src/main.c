#include <inttypes.h>
#include <stdio.h>

#include "protocol/app_protocol.h"

/* Proves the shared protocol header builds here. Replace with the CAN reader. */
int main(void)
{
    printf("CAN bitrate: %d bit/s\n", CAN_BITRATE);
    printf("Engine RPM:  0x%03X from node %u\n", CAN_ID_ENGINE_RPM,
           protocol_sensor_node_id(CAN_ID_ENGINE_RPM));
    printf("Front brake: 0x%03X from node %u\n", CAN_ID_FRONT_BRAKE,
           protocol_sensor_node_id(CAN_ID_FRONT_BRAKE));
    return 0;
}
