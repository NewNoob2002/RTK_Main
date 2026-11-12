idf_component_register(SRCS "rtk_main.c" "app_link.c" "app_cons.c"
                       INCLUDE_DIRS "."
                       REQUIRES proto link alarm cmd bt_spp store console
                                esp_driver_uart esp_timer esp_system
                                esp_hw_support)
