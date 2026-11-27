# Standalone firmware: explicit sources, no legacy or DAQ runtime dependencies.
idf_component_register(
    SRCS
        "main.cpp"
        "drive/control.cpp"
        "drive/operations.cpp"
        "canopen/drive.c"
        "canopen/feedback.cpp"
        "can/transport.cpp"
        "console/commands.cpp"
        "console/serial_console.cpp"
    INCLUDE_DIRS "." "canopen"
)
