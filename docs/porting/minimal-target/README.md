# Minimal Target Skeleton

This directory is a copyable structural example for a new hardware port. It is not a maintained production target and is intentionally not registered in the Target Registry or CI matrix.

Copy it to:

```text
firmware/targets/<target>/
```

Then customize the copied project before attempting registration.

## Files to customize

- `CMakeLists.txt`: change the ESP-IDF project name and remove shared components you do not use.
- `sdkconfig.defaults`: set the actual chip/flash/PSRAM/rollback requirements for the board.
- `partitions.csv`: size application partitions for the real flash capacity and built image.
- `main/CMakeLists.txt`: add the target's board/UI source files and component dependencies.
- `main/app_main.cpp`: fill in the target's `usage_panel::PanelTarget` implementation — the board, the display, the keys, the settings store and the screens — and name the panel in the identity.
- `main/idf_component.yml`: add one after choosing the board/BSP/component-manager dependencies.

The shared package carries what every panel has in common: `usage_panel` holds the view model, the navigation, the drawing surface and the fonts, and `usage_panel_app` holds the BLE/OTA loop. A target keeps its drivers, its keys and its screen layout.

`main/app_main.cpp` spells the `usage_panel::PanelTarget` interface out, because the skeleton has no board yet. A maintained target instantiates `usage_panel::PanelTargetImpl` with its own types instead — that wiring is shared, so it lands once.

Follow the [bring-up sequence and acceptance checklist](../../PORTING.md) before registration. This skeleton does not implement a complete BLE device; supply the board's BSP, display, input and application logic.
