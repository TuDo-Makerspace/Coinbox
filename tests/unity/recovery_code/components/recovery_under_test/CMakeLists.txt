set(COINBOX_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../")
get_filename_component(COINBOX_REPO_ROOT "${COINBOX_REPO_ROOT}" ABSOLUTE)

include("${COINBOX_REPO_ROOT}/cmake/coinbox_recovery_seed.cmake")
coinbox_load_recovery_seed(
    COINBOX_RECOVERY_SEED
    COINBOX_RECOVERY_SEED_ESCAPED
    "${COINBOX_REPO_ROOT}/.env"
)

set(RECOVERY_CODE_SCRIPT "${COINBOX_REPO_ROOT}/tools/recovery_code.py")
set(RECOVERY_CODE_VECTORS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/recovery_code_vectors.h")
set(RECOVERY_CODE_VECTOR_MACS
    "AABBCCDDEEFF"
    "000000000000"
    "112233445566"
)

file(WRITE "${RECOVERY_CODE_VECTORS_HEADER}" "#pragma once\n\n#include <stdint.h>\n\ntypedef struct {\n    uint8_t mac[6];\n    int code;\n} recovery_code_script_vector_t;\n\nstatic const recovery_code_script_vector_t RECOVERY_CODE_SCRIPT_VECTORS[] = {\n")

foreach(mac IN LISTS RECOVERY_CODE_VECTOR_MACS)
    execute_process(
        COMMAND python3 "${RECOVERY_CODE_SCRIPT}" "${COINBOX_RECOVERY_SEED}" "${mac}"
        WORKING_DIRECTORY "${COINBOX_REPO_ROOT}"
        OUTPUT_VARIABLE recovery_code_value
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE recovery_code_result
    )
    if(NOT recovery_code_result EQUAL 0)
        message(FATAL_ERROR "Failed to generate recovery-code vector for MAC ${mac}")
    endif()

    string(REGEX MATCHALL ".." mac_bytes "${mac}")
    list(GET mac_bytes 0 mac0)
    list(GET mac_bytes 1 mac1)
    list(GET mac_bytes 2 mac2)
    list(GET mac_bytes 3 mac3)
    list(GET mac_bytes 4 mac4)
    list(GET mac_bytes 5 mac5)

    file(APPEND "${RECOVERY_CODE_VECTORS_HEADER}"
         "    {{0x${mac0}, 0x${mac1}, 0x${mac2}, 0x${mac3}, 0x${mac4}, 0x${mac5}}, ${recovery_code_value}},\n")
endforeach()

file(APPEND "${RECOVERY_CODE_VECTORS_HEADER}" "};\n")

idf_component_register(
    SRCS
        "../../../../../main/recovery_code.c"
        "../../test_recovery_code.c"
    INCLUDE_DIRS
        "../../../../../main"
        "${CMAKE_CURRENT_BINARY_DIR}"
    WHOLE_ARCHIVE
    REQUIRES
        unity
        mbedtls
        log
        esp_hw_support
        esp_system
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE "COINBOX_RECOVERY_SEED=\"${COINBOX_RECOVERY_SEED_ESCAPED}\"")
