idf_component_register(SRCS "mainapp.c" "bootstrap.c" "files.c" "ota.c" "main.c" "network.c" "logger.c" "audio.c" "gpio.c" "mdns_service.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "embedded_files/favicon.ico" "embedded_files/sounds.html"
                                "embedded_files/settings.html" "embedded_files/navbar.js"
                                "embedded_files/connection_monitor.js"
                                "embedded_files/glyphs.js" "embedded_files/glyphs.css"
                                "embedded_files/bootstrap.html"
                                "embedded_files/login.html" )
