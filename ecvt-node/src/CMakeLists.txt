file(GLOB_RECURSE app_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "." "servo" "control" "inputs"
)
