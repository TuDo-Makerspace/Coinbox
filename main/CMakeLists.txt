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
    INCLUDE_DIRS
        "."
    EMBED_FILES
        ${COINBOX_EMBED_FILES}
)

if(COINBOX_EMBED_DEFINITIONS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${COINBOX_EMBED_DEFINITIONS})
endif()
