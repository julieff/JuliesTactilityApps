# Backlight Control

Adjusts the display and keyboard backlight of the LilyGO T-Deck Max through Tactility's generic `pwm-backlight` API (`backlight_set_brightness()` on the `display_backlight` and `keyboard_backlight` devices), and turns both off after a period of inactivity.

## Features

- A slider for the display backlight and one for the keyboard backlight; changes apply immediately
- An on/off switch at the top right of each backlight card turns that backlight completely off (the slider is disabled while off) and back on at the slider level
- The display slider can't reach the minimum, so the screen can't be turned off by accident
- "Turn off after inactivity": 10 seconds to 10 minutes, switchable per backlight (display, keyboard)
- Touching the screen or pressing a key turns the backlights back on
- Settings are stored in the app's user data folder (`settings.properties`)

## Structure

The package contains two apps, each built from its own subdirectory:

- `backlightcontrol`: the settings window
- `backlightidle`: a hidden, headless background service that watches LVGL input inactivity and dims or restores the backlights. It keeps running after the settings window is closed.

Shared code lives in `shared/BacklightShared.h`.

## Use

Open Backlight Control, drag the sliders, and pick an inactivity time. Choosing any time other than "Off" starts the background service; choosing "Off" stops it. The service reads the saved settings every couple of seconds, so slider changes keep applying while it runs.

## Limitations

- The service is started by opening Backlight Control. It does not start on its own after a reboot: open the app once after booting to enable the inactivity timeout again.
- On the T-Deck Max both backlight devices are disabled in the devicetree; the app starts them when first used. If the firmware has no `display_backlight` or `keyboard_backlight` device, the matching card shows "No backlight device found".
- The built-in Tactility display and keyboard idle handling may also change the backlights; turn those off in Settings if they conflict.
- Only built for ESP32-S3 and not tested on hardware.
