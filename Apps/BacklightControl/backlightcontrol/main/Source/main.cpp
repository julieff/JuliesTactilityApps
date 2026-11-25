#include "../../../shared/BacklightShared.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>
#include <app/start.h>

#include <lvgl.h>
#include <lvgl/lvgl.h>
#include <lvgl/theme.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>

namespace {

using backlight_shared::BRIGHTNESS_UNSET;

constexpr const char* TAG = "BacklightControl";
constexpr TickType_t SAVE_DEBOUNCE = pdMS_TO_TICKS(400);
constexpr int32_t TIMEOUT_VALUES[] = { 0, 10, 30, 60, 120, 300, 600 };
constexpr const char* TIMEOUT_OPTIONS = "Off\n10 seconds\n30 seconds\n1 minute\n2 minutes\n5 minutes\n10 minutes";

struct BacklightInfo {
    bool available = false;
    int32_t min = 0;
    int32_t max = 255;
    int32_t current = 0;
};

struct Context {
    AppInstanceId appInstanceId = 0;
    TaskEventGroup eventGroup {};
    uint32_t wakeBit = 0;

    BacklightInfo display;
    BacklightInfo keyboard;

    // Written by the LVGL thread and read by the app task
    std::atomic<int32_t> displayBrightness { BRIGHTNESS_UNSET };
    std::atomic<int32_t> keyboardBrightness { BRIGHTNESS_UNSET };
    std::atomic<int32_t> timeoutSeconds { 0 };
    std::atomic<bool> displayOn { true };
    std::atomic<bool> keyboardOn { true };
    std::atomic<bool> idleDisplay { true };
    std::atomic<bool> idleKeyboard { true };
    std::atomic<bool> dirty { false };
    std::atomic<TickType_t> lastChange { 0 };

    // Only touched with the LVGL lock held
    lv_obj_t* displaySlider = nullptr;
    lv_obj_t* displayLabel = nullptr;
    lv_obj_t* displaySwitch = nullptr;
    lv_obj_t* keyboardSwitch = nullptr;
    lv_obj_t* keyboardSlider = nullptr;
    lv_obj_t* keyboardLabel = nullptr;
};

BacklightInfo probe(const char* name) {
    BacklightInfo info;
    Device* device = backlight_shared::acquireBacklight(name);
    if (device == nullptr) {
        return info;
    }
    uint8_t current = 0;
    backlight_get_brightness(device, &current);
    info.available = true;
    info.min = backlight_get_min_brightness(device);
    info.max = backlight_get_max_brightness(device);
    info.current = current;
    device_put(device);
    return info;
}

void markDirty(Context* context) {
    context->lastChange.store(xTaskGetTickCount());
    context->dirty.store(true);
    task_event_group_signal(&context->eventGroup, context->wakeBit);
}

void updateLabel(lv_obj_t* label, const BacklightInfo& info, int32_t value) {
    if (label == nullptr) {
        return;
    }
    const int32_t range = info.max - info.min;
    const int32_t percent = range > 0 ? (value - info.min) * 100 / range : 0;
    lv_label_set_text_fmt(label, "%d%%", static_cast<int>(percent));
}

void onClosePressed(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(context->appInstanceId);
}

void onSliderChanged(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    auto* slider = lv_event_get_target_obj(event);
    const int32_t value = lv_slider_get_value(slider);
    const bool isDisplay = slider == context->displaySlider;

    if (isDisplay) {
        context->displayBrightness.store(value);
        updateLabel(context->displayLabel, context->display, value);
        backlight_shared::setBrightness(backlight_shared::DISPLAY_BACKLIGHT_NAME, value);
    } else {
        context->keyboardBrightness.store(value);
        updateLabel(context->keyboardLabel, context->keyboard, value);
        backlight_shared::setBrightness(backlight_shared::KEYBOARD_BACKLIGHT_NAME, value);
    }
    markDirty(context);
}

void onPowerChanged(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    auto* toggle = lv_event_get_target_obj(event);
    const bool on = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    const bool isDisplay = toggle == context->displaySwitch;
    auto* slider = isDisplay ? context->displaySlider : context->keyboardSlider;

    if (slider != nullptr) {
        // Store the slider value so the service restores it when switched on again
        const int32_t value = lv_slider_get_value(slider);
        (isDisplay ? context->displayBrightness : context->keyboardBrightness).store(value);
        if (on) {
            lv_obj_remove_state(slider, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(slider, LV_STATE_DISABLED);
        }
        const char* name = isDisplay ? backlight_shared::DISPLAY_BACKLIGHT_NAME : backlight_shared::KEYBOARD_BACKLIGHT_NAME;
        int32_t ignored = 0;
        if (on) {
            backlight_shared::setBrightness(name, value);
        } else {
            backlight_shared::turnOff(name, &ignored);
        }
    }
    (isDisplay ? context->displayOn : context->keyboardOn).store(on);
    markDirty(context);
}

void onTimeoutChanged(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    const uint32_t selected = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    context->timeoutSeconds.store(TIMEOUT_VALUES[selected]);
    markDirty(context);
}

void onIdleDisplayChanged(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    context->idleDisplay.store(lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED));
    markDirty(context);
}

void onIdleKeyboardChanged(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    context->idleKeyboard.store(lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED));
    markDirty(context);
}

lv_obj_t* createCard(lv_obj_t* parent, const char* title) {
    auto* card = lvgl_card_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    if (title != nullptr) {
        auto* label = lv_label_create(card);
        lv_label_set_text(label, title);
    }
    return card;
}

lv_obj_t* createRow(lv_obj_t* parent) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, LV_DPX(8), LV_STATE_DEFAULT);
    lv_obj_set_scrollable(row, false);
    return row;
}

void createBacklightCard(lv_obj_t* parent, Context* context, const char* title, const BacklightInfo& info,
                         int32_t configured, int32_t sliderMin, bool on, lv_obj_t** outSlider, lv_obj_t** outLabel,
                         lv_obj_t** outSwitch) {
    auto* card = createCard(parent, nullptr);
    auto* header = createRow(card);
    auto* title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_flex_grow(title_label, 1);
    if (!info.available) {
        lv_label_set_text(title_label, "No backlight device found");
        return;
    }
    auto* toggle = lv_switch_create(header);
    if (on) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(toggle, onPowerChanged, LV_EVENT_VALUE_CHANGED, context);
    *outSwitch = toggle;

    auto* row = createRow(card);
    auto* slider = lv_slider_create(row);
    lv_slider_set_range(slider, sliderMin, info.max);
    const int32_t value = configured != BRIGHTNESS_UNSET ? configured : info.current;
    lv_slider_set_value(slider, value < sliderMin ? sliderMin : value, LV_ANIM_OFF);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_add_event_cb(slider, onSliderChanged, LV_EVENT_VALUE_CHANGED, context);
    if (!on) {
        lv_obj_add_state(slider, LV_STATE_DISABLED);
    }

    auto* valueLabel = lv_label_create(row);
    lv_obj_set_width(valueLabel, LV_DPX(48));
    updateLabel(valueLabel, info, lv_slider_get_value(slider));
    *outSlider = slider;
    *outLabel = valueLabel;
}

lv_obj_t* createSwitchRow(lv_obj_t* parent, const char* text, bool checked, lv_event_cb_t callback, Context* context) {
    auto* row = createRow(parent);
    auto* label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_flex_grow(label, 1);
    auto* toggle = lv_switch_create(row);
    if (checked) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(toggle, callback, LV_EVENT_VALUE_CHANGED, context);
    return toggle;
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* context = static_cast<Context*>(userData);
    const lv_coord_t padding = lvgl_get_ui_density() == LVGL_UI_DENSITY_COMPACT ? 6 : 10;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(parent, false);

    auto* toolbar = lvgl_toolbar_create(parent, "Backlight");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onClosePressed, context);

    auto* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(content, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    // The display slider can't reach its minimum, which would turn the screen off with no way back
    createBacklightCard(content, context, "Display backlight", context->display, context->displayBrightness.load(),
                        context->display.min + 1, context->displayOn.load(), &context->displaySlider,
                        &context->displayLabel, &context->displaySwitch);
    createBacklightCard(content, context, "Keyboard backlight", context->keyboard, context->keyboardBrightness.load(),
                        context->keyboard.min, context->keyboardOn.load(), &context->keyboardSlider,
                        &context->keyboardLabel, &context->keyboardSwitch);

    auto* idleCard = createCard(content, "Turn off after inactivity");
    auto* timeout = lv_dropdown_create(idleCard);
    lv_dropdown_set_options(timeout, TIMEOUT_OPTIONS);
    uint32_t selected = 0;
    for (uint32_t i = 0; i < sizeof(TIMEOUT_VALUES) / sizeof(TIMEOUT_VALUES[0]); i++) {
        if (TIMEOUT_VALUES[i] == context->timeoutSeconds.load()) {
            selected = i;
        }
    }
    lv_dropdown_set_selected(timeout, selected);
    lv_obj_set_width(timeout, LV_PCT(100));
    lv_obj_add_event_cb(timeout, onTimeoutChanged, LV_EVENT_VALUE_CHANGED, context);
    createSwitchRow(idleCard, "Display", context->idleDisplay.load(), onIdleDisplayChanged, context);
    createSwitchRow(idleCard, "Keyboard", context->idleKeyboard.load(), onIdleKeyboardChanged, context);

    auto* hint = lv_label_create(idleCard);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "Runs in the background. Touch or press a key to turn the backlight back on.");
}

void destroyWidgets(void* userData) {
    auto* context = static_cast<Context*>(userData);
    context->displaySlider = nullptr;
    context->displayLabel = nullptr;
    context->displaySwitch = nullptr;
    context->keyboardSwitch = nullptr;
    context->keyboardSlider = nullptr;
    context->keyboardLabel = nullptr;
}

void syncIdleService(int32_t timeoutSeconds) {
    AppInstanceId instance = 0;
    const bool running = app_manager_find_running_instance(backlight_shared::IDLE_APP_ID, &instance) == ERROR_NONE;
    if (timeoutSeconds > 0 && !running) {
        AppStartContext start {};
        if (app_start_context_from_id(backlight_shared::IDLE_APP_ID, &start) != ERROR_NONE ||
            app_start_with_context(&start, &instance) != ERROR_NONE) {
            LOG_E(TAG, "Could not start the idle service");
        }
    } else if (timeoutSeconds == 0 && running) {
        app_manager_stop(instance);
    }
}

void save(Context* context) {
    backlight_shared::Settings settings;
    settings.displayBrightness = context->displayBrightness.load();
    settings.keyboardBrightness = context->keyboardBrightness.load();
    settings.timeoutSeconds = context->timeoutSeconds.load();
    settings.displayOn = context->displayOn.load();
    settings.keyboardOn = context->keyboardOn.load();
    settings.idleDisplay = context->idleDisplay.load();
    settings.idleKeyboard = context->idleKeyboard.load();
    if (!backlight_shared::saveSettings(settings)) {
        LOG_E(TAG, "Could not save settings");
    }
    syncIdleService(settings.timeoutSeconds);
}

}

extern "C" {

int main(int argc, char* argv[]) {
    Context context;
    context.appInstanceId = app_scheduler_current_app_id();
    task_event_group_construct(&context.eventGroup);
    check(task_event_group_claim_bit(&context.eventGroup, &context.wakeBit) == ERROR_NONE);

    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &context.eventGroup) == ERROR_NONE);

    const backlight_shared::Settings settings = backlight_shared::loadSettings();
    context.displayBrightness.store(settings.displayBrightness);
    context.keyboardBrightness.store(settings.keyboardBrightness);
    context.timeoutSeconds.store(settings.timeoutSeconds);
    context.displayOn.store(settings.displayOn);
    context.keyboardOn.store(settings.keyboardOn);
    context.idleDisplay.store(settings.idleDisplay);
    context.idleKeyboard.store(settings.idleKeyboard);
    context.display = probe(backlight_shared::DISPLAY_BACKLIGHT_NAME);
    context.keyboard = probe(backlight_shared::KEYBOARD_BACKLIGHT_NAME);
    syncIdleService(settings.timeoutSeconds);

    WindowId window = window_manager_create_ext(context.appInstanceId, createWidgets, destroyWidgets, &context);

    bool shouldClose = false;
    while (!shouldClose) {
        const TickType_t timeout = context.dirty.load() ? SAVE_DEBOUNCE : portMAX_DELAY;
        task_event_group_wait_any(&context.eventGroup, nullptr, timeout);

        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }

        // Save once the user stopped dragging, to keep flash writes down
        if (context.dirty.load() && xTaskGetTickCount() - context.lastChange.load() >= SAVE_DEBOUNCE) {
            context.dirty.store(false);
            save(&context);
        }
    }

    window_manager_remove(window);
    if (context.dirty.load()) {
        save(&context);
    }
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_release_bit(&context.eventGroup, context.wakeBit);
    task_event_group_destruct(&context.eventGroup);
    return 0;
}

}
