idf_component_register(SRCS "main.c"
                        "src/can.c"
                        "src/inputs.c"
                        "src/config.c"
                        "src/wifi_config.c"
                        "src/http_server.c"
                        "src/i2c_master.c"
                        "src/ads7830.c"
                        "src/http_api.c"
                    INCLUDE_DIRS "."
                    "./src"
                    "./inc")

# Create SPIFFS image from spiffs folder
spiffs_create_partition_image(spiffs ./spiffs FLASH_IN_PROJECT)
