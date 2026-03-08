include("${CMAKE_CURRENT_LIST_DIR}/../cmake/coinbox_recovery_seed.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/coinbox_version_info.cmake")
coinbox_load_recovery_seed(
    COINBOX_RECOVERY_SEED
    COINBOX_RECOVERY_SEED_ESCAPED
    "${CMAKE_CURRENT_LIST_DIR}/../.env"
)
coinbox_load_version_info(
    COINBOX_FW_VERSION
    COINBOX_FW_VERSION_ESCAPED
    COINBOX_HW_VERSION
    COINBOX_HW_VERSION_ESCAPED
    "${CMAKE_CURRENT_LIST_DIR}/.."
)

set(COINBOX_EMBED_FILES
    "embedded_files/favicon.ico"
    "embedded_files/sounds.html"
    "embedded_files/settings.html"
    "embedded_files/navbar.js"
    "embedded_files/connection_monitor.js"
    "embedded_files/glyphs.js"
    "embedded_files/glyphs.css"
    "embedded_files/bootstrap.html"
    "embedded_files/login.html"
)

set(COINBOX_EMBED_DEFINITIONS "")
list(APPEND COINBOX_EMBED_DEFINITIONS "COINBOX_RECOVERY_SEED=\"${COINBOX_RECOVERY_SEED_ESCAPED}\"")
list(APPEND COINBOX_EMBED_DEFINITIONS "COINBOX_FW_VERSION=\"${COINBOX_FW_VERSION_ESCAPED}\"")
list(APPEND COINBOX_EMBED_DEFINITIONS "COINBOX_HW_VERSION=\"${COINBOX_HW_VERSION_ESCAPED}\"")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/embedded_files/default.mp3")
    list(APPEND COINBOX_EMBED_FILES "embedded_files/default.mp3")
    list(APPEND COINBOX_EMBED_DEFINITIONS COINBOX_HAS_EMBEDDED_DEFAULT_MP3=1)
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/embedded_files/fallback.mp3")
    list(APPEND COINBOX_EMBED_FILES "embedded_files/fallback.mp3")
    list(APPEND COINBOX_EMBED_DEFINITIONS COINBOX_HAS_EMBEDDED_FALLBACK_MP3=1)
else()
    message(FATAL_ERROR "Coinbox requires embedded_files/default.mp3 or embedded_files/fallback.mp3.")
endif()

idf_component_register(
    SRCS
        "mainapp.c"
        "bootstrap.c"
        "files.c"
        "ota.c"
        "main.c"
        "network.c"
        "logger.c"
        "audio.c"
        "gpio.c"
        "mdns_service.c"
        "recovery_code.c"
        "device_info.c"
        "runtime_status.c"
        "security.c"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        ${COINBOX_EMBED_FILES}
)

if(COINBOX_EMBED_DEFINITIONS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${COINBOX_EMBED_DEFINITIONS})
endif()
