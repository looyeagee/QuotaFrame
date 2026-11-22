idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        usage_panel
        usage_panel_app
        usage_core
        usage_protocol
        usage_ble
        usage_ota
)
