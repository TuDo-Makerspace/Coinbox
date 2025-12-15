idf_component_register(SRCS "audio_test.c" "bootstrap.c" "files.c" "ota.c" "main.c" "ws_server.c" "mount.c" "network.c" "logger.c" "audio.c" "gpio.c" "mdns_service.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "embedded_files/favicon.ico" "embedded_files/fileserver.html"
                                "embedded_files/index.html" "embedded_files/mt.html" "embedded_files/navbar.js"
                                "embedded_files/bootstrap.html" "embedded_files/bootstrap_skip.html" )
