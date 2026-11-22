idf_component_register(
    SRCS "app_main.cpp" "beeper.cpp" "board.cpp" "screens.cpp" "st7305.cpp"
    INCLUDE_DIRS "."
    REQUIRES usage_panel usage_panel_app usage_core usage_protocol usage_rtc nvs_flash
             esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_i2s esp_lcd esp_adc
             esp_timer esp_system)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
