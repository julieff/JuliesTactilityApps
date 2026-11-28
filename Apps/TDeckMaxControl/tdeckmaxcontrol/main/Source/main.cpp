#include "../../../shared/TDeckMaxShared.h"

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>
#include <app/start.h>

#include <lvgl.h>
#include <lvgl/lvgl.h>
#include <lvgl/theme.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/tabs.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/device.h>
#include <tactility/drivers/radio.h>
#include <tactility/error.h>
#include <tactility/log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>

namespace {

using tdeckmax_shared::BRIGHTNESS_UNSET;

constexpr const char* TAG = "TDeckMaxControl";
constexpr TickType_t SAVE_DEBOUNCE = pdMS_TO_TICKS(400);
constexpr int32_t TIMEOUT_VALUES[] = { 0, 10, 30, 60, 120, 300, 600 };
constexpr const char* TIMEOUT_OPTIONS = "Off\n10 seconds\n30 seconds\n1 minute\n2 minutes\n5 minutes\n10 minutes";
constexpr uint32_t EXTERNAL_ANTENNA = 0;
constexpr uint32_t INTERNAL_ANTENNA = 1;

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
    bool antennaAvailable = false;
    uint32_t antenna = EXTERNAL_ANTENNA;

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
    lv_obj_t* tabs = nullptr;
    lv_obj_t* backlightPage = nullptr;
    lv_obj_t* antennaPage = nullptr;
    lv_obj_t* antennaStatus = nullptr;
    lv_obj_t* externalAntennaButton = nullptr;
    lv_obj_t* internalAntennaButton = nullptr;
};

struct AntennaRadio {
    Device* device = nullptr;
    uint32_t antenna = EXTERNAL_ANTENNA;
};

bool findAntennaRadio(Device* device, void* userData) {
    auto* found = static_cast<AntennaRadio*>(userData);
    if (device_is_compatible(device, "semtech,sx1262")) {
        found->device = device;
        return false;
    }
    return true;
}

AntennaRadio acquireAntennaRadio() {
    AntennaRadio radio;
    device_for_each_of_type(&RADIO_TYPE, &radio, findAntennaRadio);
    if (radio.device == nullptr || device_get(radio.device) != ERROR_NONE) {
        radio.device = nullptr;
        return radio;
    }
    if (!device_is_ready(radio.device) && device_start(radio.device) != ERROR_NONE) {
        device_put(radio.device);
        radio.device = nullptr;
        return radio;
    }
    if (radio_get_antenna(radio.device, &radio.antenna) != ERROR_NONE) {
        device_put(radio.device);
        radio.device = nullptr;
    }
    return radio;
}

const char* antennaName(uint32_t antenna) {
    if (antenna == EXTERNAL_ANTENNA) {
        return "External";
    }
    if (antenna == INTERNAL_ANTENNA) {
        return "Internal";
    }
    return "Unknown";
}

BacklightInfo probe(const char* name) {
    BacklightInfo info;
    Device* device = tdeckmax_shared::acquireBacklight(name);
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
        tdeckmax_shared::setBrightness(tdeckmax_shared::DISPLAY_BACKLIGHT_NAME, value);
    } else {
        context->keyboardBrightness.store(value);
        updateLabel(context->keyboardLabel, context->keyboard, value);
        tdeckmax_shared::setBrightness(tdeckmax_shared::KEYBOARD_BACKLIGHT_NAME, value);
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
        const char* name = isDisplay ? tdeckmax_shared::DISPLAY_BACKLIGHT_NAME : tdeckmax_shared::KEYBOARD_BACKLIGHT_NAME;
        if (on) {
            tdeckmax_shared::setBrightness(name, value);
        } else {
            tdeckmax_shared::turnOff(name);
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

void updateAntennaSelection(Context* context) {
    if (context->antennaStatus != nullptr) {
        if (context->antennaAvailable) {
            lv_label_set_text_fmt(context->antennaStatus, "Selected: %s antenna", antennaName(context->antenna));
        } else {
            lv_label_set_text(context->antennaStatus, "No configurable antenna switch found.");
        }
    }
    if (context->externalAntennaButton != nullptr) {
        lv_obj_set_state(context->externalAntennaButton, LV_STATE_CHECKED,
                         context->antennaAvailable && context->antenna == EXTERNAL_ANTENNA);
        lv_obj_set_state(context->externalAntennaButton, LV_STATE_DISABLED, !context->antennaAvailable);
    }
    if (context->internalAntennaButton != nullptr) {
        lv_obj_set_state(context->internalAntennaButton, LV_STATE_CHECKED,
                         context->antennaAvailable && context->antenna == INTERNAL_ANTENNA);
        lv_obj_set_state(context->internalAntennaButton, LV_STATE_DISABLED, !context->antennaAvailable);
    }
}

void onAntennaSelected(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    auto* button = lv_event_get_target_obj(event);
    const uint32_t antenna =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(button)));
    AntennaRadio radio = acquireAntennaRadio();
    if (radio.device == nullptr) {
        context->antennaAvailable = false;
        updateAntennaSelection(context);
        return;
    }

    context->antennaAvailable = true;
    context->antenna = radio.antenna;
    error_t error = ERROR_NONE;
    if (radio.antenna != antenna) {
        error = radio_set_antenna(radio.device, antenna);
    }
    device_put(radio.device);
    if (error == ERROR_NONE) {
        context->antenna = antenna;
        updateAntennaSelection(context);
        return;
    }

    updateAntennaSelection(context);
    if (error == ERROR_INVALID_STATE && context->antennaStatus != nullptr) {
        lv_label_set_text(context->antennaStatus, "Turn the radio off before switching antennas.");
    } else if (error == ERROR_NOT_SUPPORTED && context->antennaStatus != nullptr) {
        lv_label_set_text(context->antennaStatus, "Antenna switching is not supported.");
    } else if (context->antennaStatus != nullptr) {
        lv_label_set_text(context->antennaStatus, "Could not switch antennas.");
    }
    LOG_E(TAG, "Could not select antenna %u: %s", static_cast<unsigned>(antenna), error_to_string(error));
}

void onTabChanged(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    const bool showBacklight = lvgl_tabs_get_selected(context->tabs) == 0;
    lv_obj_set_hidden(context->backlightPage, !showBacklight);
    lv_obj_set_hidden(context->antennaPage, showBacklight);
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

lv_obj_t* createPage(lv_obj_t* parent, bool visible, lv_coord_t padding) {
    auto* page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), 0);
    lv_obj_set_flex_grow(page, 1);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(page, padding, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_hidden(page, !visible);
    return page;
}

lv_obj_t* createAntennaButton(lv_obj_t* parent, Context* context, const char* label, uint32_t antenna) {
    auto* button = lv_button_create(parent);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<uintptr_t>(antenna)));
    lv_obj_add_event_cb(button, onAntennaSelected, LV_EVENT_SHORT_CLICKED, context);
    lv_label_set_text(lv_label_create(button), label);
    return button;
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* context = static_cast<Context*>(userData);
    const lv_coord_t padding = lvgl_get_ui_density() == LVGL_UI_DENSITY_COMPACT ? 6 : 10;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(parent, false);

    auto* toolbar = lvgl_toolbar_create(parent, "T-Deck Max Control");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onClosePressed, context);

    auto* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(content, false);

    context->tabs = lvgl_tabs_create(content);
    lvgl_tabs_set_mode(context->tabs, LVGL_TABS_MODE_WRAP);
    lvgl_tabs_add_tab(context->tabs, "Backlight");
    lvgl_tabs_add_tab(context->tabs, "Antenna");
    lvgl_tabs_set_selected(context->tabs, 0);
    lv_obj_add_event_cb(context->tabs, onTabChanged, LV_EVENT_VALUE_CHANGED, context);

    context->backlightPage = createPage(content, true, padding);
    context->antennaPage = createPage(content, false, padding);

    // The display slider can't reach its minimum, which would turn the screen off with no way back
    createBacklightCard(context->backlightPage, context, "Display backlight", context->display,
                        context->displayBrightness.load(), context->display.min + 1, context->displayOn.load(),
                        &context->displaySlider, &context->displayLabel, &context->displaySwitch);
    createBacklightCard(context->backlightPage, context, "Keyboard backlight", context->keyboard,
                        context->keyboardBrightness.load(), context->keyboard.min, context->keyboardOn.load(),
                        &context->keyboardSlider, &context->keyboardLabel, &context->keyboardSwitch);

    auto* idleCard = createCard(context->backlightPage, "Turn off after inactivity");
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

    auto* antennaCard = createCard(context->antennaPage, "LoRa antenna");
    auto* antennaRow = createRow(antennaCard);
    context->externalAntennaButton = createAntennaButton(antennaRow, context, "External", EXTERNAL_ANTENNA);
    context->internalAntennaButton = createAntennaButton(antennaRow, context, "Internal", INTERNAL_ANTENNA);
    context->antennaStatus = lv_label_create(antennaCard);
    lv_obj_set_width(context->antennaStatus, LV_PCT(100));
    lv_label_set_long_mode(context->antennaStatus, LV_LABEL_LONG_WRAP);
    updateAntennaSelection(context);
    auto* antennaHint = lv_label_create(antennaCard);
    lv_obj_set_width(antennaHint, LV_PCT(100));
    lv_label_set_long_mode(antennaHint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(antennaHint, "The radio must be off to change antennas.");
}

void destroyWidgets(void* userData) {
    auto* context = static_cast<Context*>(userData);
    context->displaySlider = nullptr;
    context->displayLabel = nullptr;
    context->displaySwitch = nullptr;
    context->keyboardSwitch = nullptr;
    context->keyboardSlider = nullptr;
    context->keyboardLabel = nullptr;
    context->tabs = nullptr;
    context->backlightPage = nullptr;
    context->antennaPage = nullptr;
    context->antennaStatus = nullptr;
    context->externalAntennaButton = nullptr;
    context->internalAntennaButton = nullptr;
}

void syncIdleService(int32_t timeoutSeconds) {
    AppInstanceId instance = 0;
    const bool running = app_manager_find_running_instance(tdeckmax_shared::IDLE_APP_ID, &instance) == ERROR_NONE;
    if (timeoutSeconds > 0 && !running) {
        AppStartContext start {};
        if (app_start_context_from_id(tdeckmax_shared::IDLE_APP_ID, &start) != ERROR_NONE ||
            app_start_with_context(&start, &instance) != ERROR_NONE) {
            LOG_E(TAG, "Could not start the idle service");
        }
    } else if (timeoutSeconds == 0 && running) {
        app_manager_stop(instance);
    }
}

bool save(Context* context) {
    tdeckmax_shared::Settings settings;
    settings.displayBrightness = context->displayBrightness.load();
    settings.keyboardBrightness = context->keyboardBrightness.load();
    settings.timeoutSeconds = context->timeoutSeconds.load();
    settings.displayOn = context->displayOn.load();
    settings.keyboardOn = context->keyboardOn.load();
    settings.idleDisplay = context->idleDisplay.load();
    settings.idleKeyboard = context->idleKeyboard.load();
    if (!tdeckmax_shared::saveSettings(settings)) {
        LOG_E(TAG, "Could not save settings");
        return false;
    }
    syncIdleService(settings.timeoutSeconds);
    return true;
}

void applySavedBacklight(const char* name, int32_t brightness, bool on, const char* label) {
    bool applied = false;
    if (on) {
        applied = brightness == BRIGHTNESS_UNSET || tdeckmax_shared::setBrightness(name, brightness);
    } else {
        applied = tdeckmax_shared::turnOff(name);
    }
    if (!applied) {
        LOG_E(TAG, "Could not apply saved %s backlight settings", label);
    }
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

    const tdeckmax_shared::Settings settings = tdeckmax_shared::loadSettings();
    context.displayBrightness.store(settings.displayBrightness);
    context.keyboardBrightness.store(settings.keyboardBrightness);
    context.timeoutSeconds.store(settings.timeoutSeconds);
    context.displayOn.store(settings.displayOn);
    context.keyboardOn.store(settings.keyboardOn);
    context.idleDisplay.store(settings.idleDisplay);
    context.idleKeyboard.store(settings.idleKeyboard);
    context.display = probe(tdeckmax_shared::DISPLAY_BACKLIGHT_NAME);
    context.keyboard = probe(tdeckmax_shared::KEYBOARD_BACKLIGHT_NAME);
    AntennaRadio antennaRadio = acquireAntennaRadio();
    if (antennaRadio.device != nullptr) {
        context.antennaAvailable = true;
        context.antenna = antennaRadio.antenna;
        device_put(antennaRadio.device);
    }
    applySavedBacklight(tdeckmax_shared::DISPLAY_BACKLIGHT_NAME, settings.displayBrightness, settings.displayOn,
                        "display");
    applySavedBacklight(tdeckmax_shared::KEYBOARD_BACKLIGHT_NAME, settings.keyboardBrightness, settings.keyboardOn,
                        "keyboard");
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
            if (!save(&context)) {
                context.lastChange.store(xTaskGetTickCount());
                context.dirty.store(true);
            }
        }
    }

    window_manager_remove(window);
    if (context.dirty.load()) {
        context.dirty.store(false);
        if (!save(&context)) {
            context.dirty.store(true);
        }
    }
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_release_bit(&context.eventGroup, context.wakeBit);
    task_event_group_destruct(&context.eventGroup);
    return 0;
}

}
