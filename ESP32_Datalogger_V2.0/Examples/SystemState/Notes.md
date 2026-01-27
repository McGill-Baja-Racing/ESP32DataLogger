# SRC Files

## master_test.c

### Notes

### Todo

- Start Stop
- Error system

### Improvements (Items to research)

- CAN errors and esp32 errors
- gpio pin config
        - should be able to implement pullup or pulldown resistor as well as reading rising or falling edge. (see [gpio](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gpio.html))
- platformio.ini configuration (see [platformio](https://docs.platformio.org/en/latest/core/userguide/device/cmd_monitor.html#cmd-device-monitor
))
        - look into debug and release mode

### Tests

#### Control Panel

- Test 1: Blink LED (esp32p4) - DONE
        - simulate data sampling
- Test 2: Button Press (esp32p4) - DONE
        - read button -> TODO: configure to read rising edge
- Test 3: Button control of LED states (esp32p4)
- Test 4: Button control of LED (esp32p4, esp32c3)
        - button is connected to esp32p4
        - LED is connected to esp32c3
- Test 5: Task Notificaiton (esp32p4)
        - send interrupt to tell to run sampling task
- Test 6: Group event (esp32p4)
        - wake up all task after button press

#### Errors

- Test 1: Button Press (esp32p4) - DONE
        - read button -> TODO: configure to read rising edge