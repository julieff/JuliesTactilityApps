#include "TransitApi.h"

#include <app/event.h>
#include <app/scheduler.h>
#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/text_button.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>

#include <utility>

namespace {

enum class Page { Search, Results };

struct Context {
    AppInstanceId appId = 0;
    TaskEventGroup events {};
    uint32_t requestBit = 0;
    TransitRequest input {RequestKind::Connections, "", "", "", ""};
    TransitRequest pending {RequestKind::Connections, "", "", "", ""};
    TransitResult result {"Enter places and search, or find exact stops.", {}};
    RequestKind stopTarget = RequestKind::FromStops;
    Page page = Page::Search;
    bool busy = false;
    bool hasResult = false;
    lv_obj_t* root = nullptr;
    lv_timer_t* pageTimer = nullptr;
    lv_obj_t* from = nullptr;
    lv_obj_t* to = nullptr;
    lv_obj_t* date = nullptr;
    lv_obj_t* time = nullptr;
    lv_obj_t* findFrom = nullptr;
    lv_obj_t* findTo = nullptr;
    lv_obj_t* search = nullptr;
    lv_obj_t* viewResults = nullptr;
    lv_obj_t* formStatus = nullptr;
    lv_obj_t* message = nullptr;
    lv_obj_t* stops = nullptr;
};

void renderResult(Context* ctx);
void createWidgets(lv_obj_t* parent, void* userData);

void onPageTimer(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    ctx->pageTimer = nullptr;
    createWidgets(ctx->root, ctx);
}

void showPage(Context* ctx, Page page) {
    ctx->page = page;
    if (ctx->root && !ctx->pageTimer) {
        ctx->pageTimer = lv_timer_create(onPageTimer, 0, ctx);
        lv_timer_set_repeat_count(ctx->pageTimer, 1);
    }
}

void onInput(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    lv_obj_t* field = lv_event_get_target_obj(event);
    if (field == ctx->from) ctx->input.from = lv_textarea_get_text(field);
    else if (field == ctx->to) ctx->input.to = lv_textarea_get_text(field);
    else if (field == ctx->date) ctx->input.date = lv_textarea_get_text(field);
    else if (field == ctx->time) ctx->input.time = lv_textarea_get_text(field);
}

void onStopSelected(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    uint32_t index = lv_obj_get_index(lv_event_get_target_obj(event));
    if (index >= ctx->result.stops.size()) return;
    if (ctx->stopTarget == RequestKind::FromStops) {
        ctx->input.from = ctx->result.stops[index];
    } else {
        ctx->input.to = ctx->result.stops[index];
    }
    ctx->result.stops.clear();
    ctx->hasResult = false;
    showPage(ctx, Page::Search);
}

void renderResult(Context* ctx) {
    if (ctx->formStatus) {
        lv_label_set_text(ctx->formStatus, ctx->busy ? "Searching..." :
                          ctx->hasResult ? "Select View results to reopen the last search." : "");
    }
    if (ctx->findFrom) lv_obj_set_state(ctx->findFrom, LV_STATE_DISABLED, ctx->busy);
    if (ctx->findTo) lv_obj_set_state(ctx->findTo, LV_STATE_DISABLED, ctx->busy);
    if (ctx->search) lv_obj_set_state(ctx->search, LV_STATE_DISABLED, ctx->busy);
    if (ctx->viewResults) lv_obj_set_hidden(ctx->viewResults, !ctx->hasResult);
    if (!ctx->message || !ctx->stops) return;
    lv_label_set_text(ctx->message, ctx->result.message.c_str());
    lv_obj_clean(ctx->stops);
    for (const std::string& stop : ctx->result.stops) {
        lv_obj_t* button = lv_button_create(ctx->stops);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, stop.c_str());
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_add_event_cb(button, onStopSelected, LV_EVENT_CLICKED, ctx);
    }
}

void startRequest(Context* ctx, RequestKind kind) {
    if (ctx->busy) return;
    ctx->stopTarget = kind;
    ctx->hasResult = true;
    if (kind == RequestKind::Connections && (ctx->input.from.empty() || ctx->input.to.empty())) {
        ctx->result = {"Enter both origin and destination.", {}};
    } else if (kind != RequestKind::Connections &&
               (kind == RequestKind::FromStops ? ctx->input.from : ctx->input.to).size() < 2) {
        ctx->result = {"Enter at least two characters to find stops.", {}};
    } else {
        ctx->pending = ctx->input;
        ctx->pending.kind = kind;
        ctx->busy = true;
        ctx->result = {"Searching...", {}};
        if (task_event_group_signal(&ctx->events, ctx->requestBit) != ERROR_NONE) {
            ctx->busy = false;
            ctx->result.message = "Could not start the search.";
        }
    }
    renderResult(ctx);
    showPage(ctx, Page::Results);
}

void onSearch(lv_event_t* event) {
    startRequest(static_cast<Context*>(lv_event_get_user_data(event)), RequestKind::Connections);
}

void onFindFrom(lv_event_t* event) {
    startRequest(static_cast<Context*>(lv_event_get_user_data(event)), RequestKind::FromStops);
}

void onFindTo(lv_event_t* event) {
    startRequest(static_cast<Context*>(lv_event_get_user_data(event)), RequestKind::ToStops);
}

void onClose(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appId);
}

void onBack(lv_event_t* event) {
    showPage(static_cast<Context*>(lv_event_get_user_data(event)), Page::Search);
}

void onViewResults(lv_event_t* event) {
    showPage(static_cast<Context*>(lv_event_get_user_data(event)), Page::Results);
}

lv_obj_t* addField(lv_obj_t* parent, const char* placeholder, const std::string& value, Context* ctx) {
    lv_obj_t* field = lv_textarea_create(parent);
    lv_obj_set_width(field, LV_PCT(100));
    lv_textarea_set_one_line(field, true);
    lv_textarea_set_max_length(field, 100);
    lv_textarea_set_placeholder_text(field, placeholder);
    lv_textarea_set_text(field, value.c_str());
    lv_obj_add_event_cb(field, onInput, LV_EVENT_VALUE_CHANGED, ctx);
    return field;
}

lv_obj_t* addButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback, Context* ctx) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_width(button, LV_PCT(100));
    lv_label_set_text(lv_label_create(button), text);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ctx);
    return button;
}

void addStopRow(lv_obj_t* parent, const char* placeholder, const std::string& value,
                lv_event_cb_t callback, Context* ctx, lv_obj_t** field, lv_obj_t** button) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, LV_DPX(2), LV_STATE_DEFAULT);
    lv_obj_set_scrollable(row, false);

    *field = addField(row, placeholder, value, ctx);
    lv_obj_set_width(*field, 0);
    lv_obj_set_flex_grow(*field, 1);
    *button = lvgl_text_button_create(row);
    lv_obj_set_size(*button, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(*button, LV_DPX(2), LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(*button, LV_DPX(2), LV_STATE_DEFAULT);
    lv_obj_t* label = lv_label_create(*button);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_SMALL), LV_STATE_DEFAULT);
    lv_label_set_text(label, "Find stops");
    lv_obj_add_event_cb(*button, callback, LV_EVENT_CLICKED, ctx);
}

void clearWidgetPointers(Context* ctx) {
    ctx->from = nullptr;
    ctx->to = nullptr;
    ctx->date = nullptr;
    ctx->time = nullptr;
    ctx->findFrom = nullptr;
    ctx->findTo = nullptr;
    ctx->search = nullptr;
    ctx->viewResults = nullptr;
    ctx->formStatus = nullptr;
    ctx->message = nullptr;
    ctx->stops = nullptr;
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->root = parent;
    clearWidgetPointers(ctx);
    lv_obj_clean(parent);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(parent, false);
    lv_obj_t* toolbar = lvgl_toolbar_create(parent, ctx->page == Page::Search ? "Slovakia Transit" : "Results");
    lv_obj_set_width(toolbar, LV_PCT(100));
    lvgl_toolbar_set_nav_action(toolbar, ctx->page == Page::Search ? LV_SYMBOL_CLOSE : LV_SYMBOL_LEFT,
                               ctx->page == Page::Search ? onClose : onBack, ctx);

    lv_obj_t* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_height(content, 0);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_radius(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(content, LV_DPX(4), LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    if (ctx->page == Page::Search) {
        addStopRow(content, "From", ctx->input.from, onFindFrom, ctx, &ctx->from, &ctx->findFrom);
        addStopRow(content, "To", ctx->input.to, onFindTo, ctx, &ctx->to, &ctx->findTo);
        ctx->date = addField(content, "Date: YYYY-MM-DD (optional)", ctx->input.date, ctx);
        ctx->time = addField(content, "Time: HH:MM (optional)", ctx->input.time, ctx);
        ctx->search = addButton(content, "Search bus + train", onSearch, ctx);
        ctx->formStatus = lv_label_create(content);
        lv_obj_set_width(ctx->formStatus, LV_PCT(100));
        lv_label_set_long_mode(ctx->formStatus, LV_LABEL_LONG_MODE_WRAP);
        ctx->viewResults = addButton(content, "View results", onViewResults, ctx);
    } else {
        lv_obj_add_flag(content, LV_OBJ_FLAG_CLICKABLE);
        lv_group_t* group = lv_group_get_default();
        if (group) lv_group_add_obj(group, content);
        ctx->message = lv_label_create(content);
        lv_obj_set_width(ctx->message, LV_PCT(100));
        lv_label_set_long_mode(ctx->message, LV_LABEL_LONG_MODE_WRAP);
        ctx->stops = lv_obj_create(content);
        lv_obj_remove_style_all(ctx->stops);
        lv_obj_set_width(ctx->stops, LV_PCT(100));
        lv_obj_set_height(ctx->stops, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(ctx->stops, LV_FLEX_FLOW_COLUMN);
    }
    renderResult(ctx);
}

void destroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    if (ctx->pageTimer) {
        lv_timer_delete(ctx->pageTimer);
        ctx->pageTimer = nullptr;
    }
    ctx->root = nullptr;
    clearWidgetPointers(ctx);
}

} // namespace

extern "C" int main(int argc, char* argv[]) {
    Context ctx;
    ctx.appId = app_scheduler_current_app_id();
    task_event_group_construct(&ctx.events);
    check(task_event_group_claim_bit(&ctx.events, &ctx.requestBit) == ERROR_NONE);
    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &ctx.events) == ERROR_NONE);
    WindowId window = window_manager_create_ext(ctx.appId, createWidgets, destroyWidgets, &ctx);

    bool close = false;
    while (!close) {
        uint32_t flags = 0;
        task_event_group_wait_any(&ctx.events, &flags, portMAX_DELAY);
        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) close = true;
        }
        if (close) break;
        if (flags & ctx.requestBit) {
            lvgl_lock();
            TransitRequest request = ctx.pending;
            lvgl_unlock();
            TransitResult result = transitFetch(request);
            if (request.kind == RequestKind::Connections) {
                result.message = request.from + " -> " + request.to + "\n" + result.message;
            } else {
                const std::string& query = request.kind == RequestKind::FromStops ? request.from : request.to;
                result.message = "Stops for " + query + "\n" + result.message;
            }
            lvgl_lock();
            ctx.result = std::move(result);
            ctx.stopTarget = request.kind;
            ctx.busy = false;
            renderResult(&ctx);
            lvgl_unlock();
        }
    }
    window_manager_remove(window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_release_bit(&ctx.events, ctx.requestBit);
    task_event_group_destruct(&ctx.events);
    return 0;
}
