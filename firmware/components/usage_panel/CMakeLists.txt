idf_component_register(
    SRCS
        "src/canvas.cpp"
        "src/cjk_font.cpp"
        "src/panel_logic.cpp"
        "src/screen_parts.cpp"
        "src/cjk_font_data.cpp"
    INCLUDE_DIRS "include"
    REQUIRES usage_core
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
