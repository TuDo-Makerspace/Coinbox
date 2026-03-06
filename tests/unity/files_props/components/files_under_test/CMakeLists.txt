idf_component_register(
    SRCS
        "../../../../../main/files.c"
        "../../test_files_props.c"
    INCLUDE_DIRS
        "../../../../../main"
    WHOLE_ARCHIVE
    REQUIRES
        unity
        joltwallet__littlefs
        log
        esp_system
)

# files.c expects this project Kconfig value from the main firmware.
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_BASE_PATH=\"/data\")
