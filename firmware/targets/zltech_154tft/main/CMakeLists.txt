idf_component_register(
    SRCS "app_main.cpp" "beeper.cpp" "board.cpp" "canvas.cpp" "cjk_font.cpp"
         "cjk_font_data.cpp" "panel_logic.cpp" "screens.cpp" "st7789.cpp"
    INCLUDE_DIRS "."
    REQUIRES usage_core usage_ble usage_ota nvs_flash esp_driver_spi esp_driver_gpio
             esp_driver_ledc esp_lcd esp_adc esp_timer esp_system)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
