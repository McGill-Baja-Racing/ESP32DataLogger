#ifdef RPM_NODE
#include "rpm_node.hpp"
#elif MASTER
#include "master_btn_control.hpp"
#elif SENSOR_TEMPLATE
#include "test.hpp"
#endif
#include "adc_oneshot_node.hpp"


void app_main() {
    #ifdef RPM_NODE
        rpm_node_main();
    #elif MASTER
        btn_control_main();
    #elif ADC_ONESHOT
        adc_oneshot_main();
    #elif SENSOR_TEMPLATE
        node_main();
    #endif
}


