#ifdef RPM_NODE
#include "rpm_node.hpp"
#elif ADC_ONESHOT
#include "adc_oneshot_node.hpp"
#endif

void app_main() {
    #ifdef RPM_NODE
        rpm_node_main();
    #elif ADC_ONESHOT
        adc_oneshot_main();
    #endif
}


