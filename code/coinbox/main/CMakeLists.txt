idf_component_register(SRCS "ota.c" "main.c" "ws_server.c" "mount.c" "network.c" "ota.c" "logger.c" "audio.c" "gpio.c" "file_entry.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "embedded_files/favicon.ico" "embedded_files/fileserver.html" 
                                "embedded_files/index.html" "embedded_files/mt.html" "embedded_files/navbar.js" )
