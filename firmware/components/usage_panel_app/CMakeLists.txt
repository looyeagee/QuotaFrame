idf_component_register(
    SRCS "src/app_runtime.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        usage_panel
        usage_ble
        usage_ota
        usage_protocol
        nvs_flash
        esp_timer
        esp_system
        esp_hw_support
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
