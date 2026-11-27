# Explicit firmware boundary: standalone motor control and terminal only.
idf_component_register(
    SRCS
        "main.cpp"
        "app/ecvt_app.cpp"
        "can/can_transport.cpp"
        "console/command_parser.cpp"
        "console/serial_console.cpp"
        "motor/motor_core.cpp"
        "motor/drive_operations.cpp"
        "motor/parameters.cpp"
        "experiments/motor_tests.cpp"
        "experiments/brake_probe.cpp"
        "servo/ECVT_motor.c"
        "servo/drive_state.cpp"
        "servo/servo_feedback.cpp"
        "servo/velocity_zero.cpp"
    INCLUDE_DIRS "." "servo"
)
