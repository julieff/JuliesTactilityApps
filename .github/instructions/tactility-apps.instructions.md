---
applyTo: "Apps/**"
---

# Tactility apps

- Follow the [app-development documentation](https://github.com/TactilityProject/TactilityDocs/tree/main/docs/main/app-development), particularly the fundamentals, UI, input, and icon guides.
- Keep each app self-contained with its own manifest, CMake files, and source.
  Use the SDK version targeted by its manifest and only APIs exported by the
  matching firmware.
- Give every app with a UI both `icon.svg` and `icon_mono.svg` beside its
  manifest. Follow the icon-specific repository instructions.
- Use the window manager and keep app state outside the widgets: windows can be
  destroyed and rebuilt. Clear cached widget pointers when widgets are destroyed.
- Use the LVGL lock when updating widgets from an app task. Do not perform file
  or network operations on the LVGL thread or while holding the LVGL lock.
- Make layouts adapt to screen sizes and keep interactive controls usable with
  both touch and keyboard or directional input.
- Handle service and resource errors explicitly. Document external services,
  connectivity requirements, and important limitations.
