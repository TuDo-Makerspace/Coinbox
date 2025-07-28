idf_component_register(SRCS "main.c" "ws_server.c" "mount.c" "network.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "embedded_files/favicon.ico" "embedded_files/upload_script.html" 
                                "embedded_files/index.html" "embedded_files/mt.html" )
