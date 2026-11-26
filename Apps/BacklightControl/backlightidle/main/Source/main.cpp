#include "../../../shared/BacklightShared.h"

#include <app/event.h>

#include <lvgl.h>
#include <lvgl/lvgl.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/log.h>

namespace {

constexpr const char* TAG = "BacklightIdle";
constexpr TickType_t TICK_PERIOD = pdMS_TO_TICKS(100);
constexpr int SETTINGS_RELOAD_TICKS = 20;
// Input newer than this wakes the backlights again
constexpr uint32_t WAKE_THRESHOLD_MS = 300;

struct Target {
    const char* name;
    bool dimmed = false;
    int32_t previous = backlight_shared::BRIGHTNESS_UNSET;
};

uint32_t getInactiveMs() {
    // Treat a busy UI as active input
    uint32_t inactive = 0;
    if (lvgl_try_lock(100)) {
        inactive = lv_display_get_inactive_time(nullptr);
        lvgl_unlock();
    }
    return inactive;
}

void apply(Target& target, int32_t brightness, bool on) {
    if (!on) {
        backlight_shared::turnOff(target.name);
    } else if (brightness != backlight_shared::BRIGHTNESS_UNSET) {
        backlight_shared::setBrightness(target.name, brightness);
    }
}

void restore(Target& target, int32_t configured, bool on) {
    if (!on) {
        if (backlight_shared::turnOff(target.name)) {
            target.dimmed = false;
        }
        return;
    }
    const int32_t brightness = configured != backlight_shared::BRIGHTNESS_UNSET ? configured : target.previous;
    if (brightness != backlight_shared::BRIGHTNESS_UNSET &&
        backlight_shared::setBrightness(target.name, brightness)) {
        target.dimmed = false;
    }
}

void update(Target& target, bool enabled, bool idle, bool active, int32_t configured, bool on) {
    if (target.dimmed && (!enabled || active)) {
        restore(target, configured, on);
    } else if (!target.dimmed && enabled && idle) {
        target.previous = backlight_shared::BRIGHTNESS_UNSET;
        if (backlight_shared::turnOffAndGetPrevious(target.name, &target.previous)) {
            target.dimmed = true;
        }
    }
}

}

extern "C" {

int main(int argc, char* argv[]) {
    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &eventGroup) == ERROR_NONE);

    backlight_shared::Settings settings = backlight_shared::loadSettings();
    Target display { backlight_shared::DISPLAY_BACKLIGHT_NAME };
    Target keyboard { backlight_shared::KEYBOARD_BACKLIGHT_NAME };

    apply(display, settings.displayBrightness, settings.displayOn);
    apply(keyboard, settings.keyboardBrightness, settings.keyboardOn);
    LOG_I(TAG, "Started, timeout %ds", static_cast<int>(settings.timeoutSeconds));

    bool shouldClose = false;
    int ticksUntilReload = SETTINGS_RELOAD_TICKS;
    uint32_t lastInactive = 0;
    while (!shouldClose) {
        task_event_group_wait_any(&eventGroup, nullptr, TICK_PERIOD);

        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
        if (shouldClose) {
            break;
        }

        if (--ticksUntilReload <= 0) {
            ticksUntilReload = SETTINGS_RELOAD_TICKS;
            const backlight_shared::Settings loaded = backlight_shared::loadSettings();
            // A brightness picked in the settings app applies to a backlight that is on
            if (!display.dimmed && (loaded.displayBrightness != settings.displayBrightness ||
                                    loaded.displayOn != settings.displayOn)) {
                apply(display, loaded.displayBrightness, loaded.displayOn);
            }
            if (!keyboard.dimmed && (loaded.keyboardBrightness != settings.keyboardBrightness ||
                                     loaded.keyboardOn != settings.keyboardOn)) {
                apply(keyboard, loaded.keyboardBrightness, loaded.keyboardOn);
            }
            settings = loaded;
        }

        const bool timeoutEnabled = settings.timeoutSeconds > 0;
        const uint32_t inactive = getInactiveMs();
        const bool idle = timeoutEnabled && inactive >= static_cast<uint32_t>(settings.timeoutSeconds) * 1000U;
        // A counter that went down means input happened since the previous tick
        const bool active = inactive < WAKE_THRESHOLD_MS || inactive < lastInactive;
        lastInactive = inactive;
        update(display, timeoutEnabled && settings.idleDisplay && settings.displayOn, idle, active,
               settings.displayBrightness, settings.displayOn);
        update(keyboard, timeoutEnabled && settings.idleKeyboard && settings.keyboardOn, idle, active,
               settings.keyboardBrightness, settings.keyboardOn);
    }

    // The settings app turns the timeout off before it stops this service. A close while the timeout
    // is still enabled comes from the system (e.g. shutdown), so leave the backlights off instead of
    // lighting them up again.
    settings = backlight_shared::loadSettings();
    if (settings.timeoutSeconds > 0) {
        apply(display, settings.displayBrightness, false);
        apply(keyboard, settings.keyboardBrightness, false);
    } else {
        if (display.dimmed) restore(display, settings.displayBrightness, settings.displayOn);
        if (keyboard.dimmed) restore(keyboard, settings.keyboardBrightness, settings.keyboardOn);
    }

    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);
    return 0;
}

}
