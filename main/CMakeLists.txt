idf_component_register(SRCS "webserver-esp32.c" "wifi_config_portal.c"
                    INCLUDE_DIRS ".")
spiffs_create_partition_image(storage ../data FLASH_IN_PROJECT)