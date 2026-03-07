set(COINBOX_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../")
get_filename_component(COINBOX_REPO_ROOT "${COINBOX_REPO_ROOT}" ABSOLUTE)

set(COINBOX_DEFAULT_SOUND_EMBED_FILE "")
set(COINBOX_DEFAULT_SOUND_EMBED_DEFINITION "")
if(EXISTS "${COINBOX_REPO_ROOT}/main/embedded_files/default.mp3")
    set(COINBOX_DEFAULT_SOUND_EMBED_FILE "${COINBOX_REPO_ROOT}/main/embedded_files/default.mp3")
    set(COINBOX_DEFAULT_SOUND_EMBED_DEFINITION "COINBOX_HAS_EMBEDDED_DEFAULT_MP3=1")
elseif(EXISTS "${COINBOX_REPO_ROOT}/main/embedded_files/fallback.mp3")
    set(COINBOX_DEFAULT_SOUND_EMBED_FILE "${COINBOX_REPO_ROOT}/main/embedded_files/fallback.mp3")
    set(COINBOX_DEFAULT_SOUND_EMBED_DEFINITION "COINBOX_HAS_EMBEDDED_FALLBACK_MP3=1")
else()
    message(FATAL_ERROR "Coinbox requires main/embedded_files/default.mp3 or main/embedded_files/fallback.mp3.")
endif()

idf_component_register(
    SRCS
        "../../../../../main/files.c"
        "../../test_files_props.c"
    INCLUDE_DIRS
        "../../../../../main"
    EMBED_FILES
        "${COINBOX_DEFAULT_SOUND_EMBED_FILE}"
    WHOLE_ARCHIVE
    REQUIRES
        unity
        joltwallet__littlefs
        log
        esp_system
)

# files.c expects this project Kconfig value from the main firmware.
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_BASE_PATH=\"/data\")
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${COINBOX_DEFAULT_SOUND_EMBED_DEFINITION})
