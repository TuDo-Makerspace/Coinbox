set(COINBOX_FW_VERSION_DEFAULT "0.0.0")
set(COINBOX_HW_VERSION_DEFAULT "0.0.0")

function(_coinbox_escape_c_string input output_var)
    set(value "${input}")
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${output_var} "${value}" PARENT_SCOPE)
endfunction()

function(coinbox_load_version_info out_fw out_fw_escaped out_hw out_hw_escaped source_dir)
    set(fw_version "${COINBOX_FW_VERSION_DEFAULT}")
    set(hw_version "${COINBOX_HW_VERSION_DEFAULT}")

    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" describe --tags --abbrev=0
            RESULT_VARIABLE git_result
            OUTPUT_VARIABLE git_tag
            ERROR_VARIABLE git_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(git_result EQUAL 0)
            string(STRIP "${git_tag}" git_tag)
            if(git_tag MATCHES "^fw([0-9]+\\.[0-9]+\\.[0-9]+)-hw([0-9]+\\.[0-9]+\\.[0-9]+)$")
                set(fw_version "${CMAKE_MATCH_1}")
                set(hw_version "${CMAKE_MATCH_2}")
            else()
                message(WARNING
                    "Coinbox latest tag '${git_tag}' does not match 'fwx.y.z-hwx.y.z'; "
                    "using default firmware/hardware versions.")
            endif()
        else()
            message(WARNING
                "Coinbox could not read latest git tag from '${source_dir}': ${git_error}; "
                "using default firmware/hardware versions.")
        endif()
    else()
        message(WARNING "Git not found; using default Coinbox firmware/hardware versions.")
    endif()

    _coinbox_escape_c_string("${fw_version}" fw_version_escaped)
    _coinbox_escape_c_string("${hw_version}" hw_version_escaped)

    set(${out_fw} "${fw_version}" PARENT_SCOPE)
    set(${out_fw_escaped} "${fw_version_escaped}" PARENT_SCOPE)
    set(${out_hw} "${hw_version}" PARENT_SCOPE)
    set(${out_hw_escaped} "${hw_version_escaped}" PARENT_SCOPE)
endfunction()
