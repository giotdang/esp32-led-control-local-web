idf_component_register(SRCS "main.c" "wifi_manager.c" "captive_portal.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "web/led_controller.html"
                                   "web/wifi_config.html"
                                   "web/configure_success.html")
