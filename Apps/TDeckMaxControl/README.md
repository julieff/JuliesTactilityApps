# T-Deck Max Control

Controls the display and keyboard backlight of the LilyGO T-Deck Max and selects its LoRa antenna.

## Features

- The **Backlight** tab has display and keyboard brightness sliders, power switches, and inactivity controls.
- The **Antenna** tab switches between the internal and external LoRa antennas through Tactility's radio API.
- Backlight settings are stored in the app's user data folder (`settings.properties`).

## Structure

The package contains two apps, each built from its own subdirectory:

- `tdeckmaxcontrol`: the T-Deck Max Control settings window.
- `tdeckmaxidle`: a hidden, headless background service that watches LVGL input inactivity and dims or restores the backlights.

Shared code lives in `shared/TDeckMaxShared.h`.

## Use

Open **T-Deck Max Control** and choose a tab. Backlight changes apply immediately. Choosing an inactivity time other than "Off" starts the background service; choosing "Off" stops it. The service keeps running after the settings window closes. In **Antenna**, choose Internal or External. The radio must be off to switch antennas.

## Limitations

- Building requires a Tactility SDK generated from firmware that exports `radio_get_antenna()` and `radio_set_antenna()`, such as the `antena-switching` branch. The installed `0.8.0-dev` SDK may not include these APIs despite having the same version.
- Antenna selection requires matching firmware with the radio antenna API and a configured antenna switch. The selection returns to the device-tree default after reboot.
- The package ID is `tactility.tdeckmaxcontrol`; existing backlight settings are loaded from the previous package ID when needed.
- The inactivity service starts when the app is opened and a timeout is enabled; it does not restart itself after reboot.
- If the firmware has no `display_backlight` or `keyboard_backlight` device, the matching backlight card shows "No backlight device found".
- Built-in Tactility display and keyboard idle handling may also change the backlights; turn those off in Settings if they conflict.
- Built for ESP32-S3 and not tested on hardware.
