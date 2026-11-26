#pragma once

// Code shared by the settings app and the headless idle service of this package.

#include <app/paths.h>

#include <tactility/device.h>
#include <tactility/drivers/backlight.h>
#include <tactility/preferences.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace backlight_shared {

constexpr const char* PACKAGE_ID = "tactility.backlightcontrol";
constexpr const char* IDLE_APP_ID = "tactility.backlightcontrol.idle";
constexpr const char* DISPLAY_BACKLIGHT_NAME = "display_backlight";
constexpr const char* KEYBOARD_BACKLIGHT_NAME = "keyboard_backlight";

/** A brightness of -1 means the user never chose one: the current hardware value is left alone. */
constexpr int32_t BRIGHTNESS_UNSET = -1;

struct Settings {
    int32_t displayBrightness = BRIGHTNESS_UNSET;
    int32_t keyboardBrightness = BRIGHTNESS_UNSET;
    /** Inactivity time in seconds before the backlights turn off. 0 disables the feature. */
    int32_t timeoutSeconds = 0;
    bool displayOn = true;
    bool keyboardOn = true;
    bool idleDisplay = true;
    bool idleKeyboard = true;
};

inline bool settingsPath(char* out, size_t size) {
    return app_paths_get_user_data_path(PACKAGE_ID, "settings.properties", out, size) == ERROR_NONE;
}

/** Performs file I/O: never call it on the LVGL thread. */
inline Settings loadSettings() {
    Settings settings;
    char path[256];
    if (!settingsPath(path, sizeof(path))) {
        return settings;
    }
    Preferences* prefs = preferences_open(path);
    if (prefs == nullptr) {
        return settings;
    }
    preferences_opt_int32(prefs, "display_brightness", &settings.displayBrightness);
    preferences_opt_int32(prefs, "keyboard_brightness", &settings.keyboardBrightness);
    preferences_opt_int32(prefs, "timeout_seconds", &settings.timeoutSeconds);
    preferences_opt_bool(prefs, "display_on", &settings.displayOn);
    preferences_opt_bool(prefs, "keyboard_on", &settings.keyboardOn);
    preferences_opt_bool(prefs, "idle_display", &settings.idleDisplay);
    preferences_opt_bool(prefs, "idle_keyboard", &settings.idleKeyboard);
    preferences_close(prefs);
    return settings;
}

/** Performs file I/O: never call it on the LVGL thread. */
inline bool saveSettings(const Settings& settings) {
    char path[256];
    if (!settingsPath(path, sizeof(path))) {
        return false;
    }
    Preferences* prefs = preferences_open(path);
    if (prefs == nullptr) {
        return false;
    }
    preferences_put_int32(prefs, "display_brightness", settings.displayBrightness);
    preferences_put_int32(prefs, "keyboard_brightness", settings.keyboardBrightness);
    preferences_put_int32(prefs, "timeout_seconds", settings.timeoutSeconds);
    preferences_put_bool(prefs, "display_on", settings.displayOn);
    preferences_put_bool(prefs, "keyboard_on", settings.keyboardOn);
    preferences_put_bool(prefs, "idle_display", settings.idleDisplay);
    preferences_put_bool(prefs, "idle_keyboard", settings.idleKeyboard);
    preferences_close(prefs);
    return true;
}

struct FindContext {
    const char* name;
    Device* found;
};

inline bool findBacklight(Device* device, void* context) {
    auto* find = static_cast<FindContext*>(context);
    if (device->name != nullptr && strcmp(device->name, find->name) == 0) {
        find->found = device;
        return false;
    }
    return true;
}

/**
 * Looks up a pwm-backlight device by its devicetree node name and starts it when needed (these
 * devices are disabled by default in the devicetree). Returns null when it doesn't exist or fails
 * to start. A non-null result holds a reference that must be released with device_put().
 */
inline Device* acquireBacklight(const char* name) {
    FindContext find { name, nullptr };
    device_for_each_of_type(&BACKLIGHT_TYPE, &find, findBacklight);
    if (find.found == nullptr || device_get(find.found) != ERROR_NONE) {
        return nullptr;
    }
    if (!device_is_ready(find.found) && device_start(find.found) != ERROR_NONE) {
        device_put(find.found);
        return nullptr;
    }
    return find.found;
}

inline bool setBrightness(const char* name, int32_t brightness) {
    Device* device = acquireBacklight(name);
    if (device == nullptr) {
        return false;
    }
    const int32_t min = backlight_get_min_brightness(device);
    const int32_t max = backlight_get_max_brightness(device);
    if (brightness < min) brightness = min;
    if (brightness > max) brightness = max;
    const bool ok = backlight_set_brightness(device, static_cast<uint8_t>(brightness)) == ERROR_NONE;
    device_put(device);
    return ok;
}

/** Sets the minimum brightness (off). */
inline bool turnOff(const char* name) {
    Device* device = acquireBacklight(name);
    if (device == nullptr) {
        return false;
    }
    const bool ok = backlight_set_brightness(device, backlight_get_min_brightness(device)) == ERROR_NONE;
    device_put(device);
    return ok;
}

/** Dims to minimum brightness and reports the prior brightness for later restoration. */
inline bool turnOffAndGetPrevious(const char* name, int32_t* out_previous) {
    Device* device = acquireBacklight(name);
    if (device == nullptr) {
        return false;
    }
    uint8_t current = 0;
    if (backlight_get_brightness(device, &current) != ERROR_NONE) {
        device_put(device);
        return false;
    }
    *out_previous = current;
    const bool ok = backlight_set_brightness(device, backlight_get_min_brightness(device)) == ERROR_NONE;
    device_put(device);
    return ok;
}

}
