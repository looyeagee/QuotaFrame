idf_component_register(
    SRCS "app_main.cpp" "axp2101.cpp" "beeper.cpp" "board.cpp" "screens.cpp" "sh8601.cpp"
    INCLUDE_DIRS "."
    REQUIRES usage_panel usage_panel_app usage_core usage_protocol usage_rtc nvs_flash
             esp_driver_i2c esp_driver_spi esp_driver_gpio esp_lcd esp_timer esp_system
             espressif__esp_lcd_sh8601 espressif__esp_lcd_touch_ft5x06 espressif__esp_lcd_touch
             espressif__esp_io_expander espressif__esp_io_expander_tca9554)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
