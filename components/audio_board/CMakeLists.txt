set(COMPONENT_SRCS "board.c" "board_pins_config.c")
set(COMPONENT_ADD_INCLUDEDIRS "include")
set(COMPONENT_REQUIRES)
set(COMPONENT_PRIV_REQUIRES esp_peripherals audio_sal audio_hal esp_dispatcher display_service)

register_component()
