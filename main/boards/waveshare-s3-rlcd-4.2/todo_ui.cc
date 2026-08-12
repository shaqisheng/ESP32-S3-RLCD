// REFERENCE_TODO_LIST_V1
// 紧凑列表：顶部汇总 + 5 行未完成待办 + 可选 overflow 指示。
// 见 admin 后台 'todo' tab 的设备端对应物。

#include "custom_lcd_display.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "managers/todo_manager.h"

LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);

namespace {
constexpr int kTodoHeaderY = 6;
constexpr int kTodoHeaderHeight = 30;
constexpr int kTodoRowY = 44;
constexpr int kTodoRowHeight = 30;
constexpr int kTodoMaxRows = 5;
constexpr int kTodoOverflowY = kTodoRowY + kTodoMaxRows * kTodoRowHeight + 4;

void MakePlain(lv_obj_t* object, lv_color_t color) {
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}
}  // namespace

void CustomLcdDisplay::SetupTodoUI() {
    DisplayLockGuard lock(this);
    todo_page_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(todo_page_, 400, 300);
    lv_obj_set_pos(todo_page_, 0, 0);
    MakePlain(todo_page_, lv_color_white());

    todo_header_label_ = lv_label_create(todo_page_);
    lv_obj_set_pos(todo_header_label_, 12, kTodoHeaderY);
    lv_obj_set_size(todo_header_label_, 376, kTodoHeaderHeight);
    lv_obj_set_style_text_font(todo_header_label_, &alibaba_puhui_24, 0);
    lv_obj_set_style_text_color(todo_header_label_, lv_color_black(), 0);
    lv_label_set_text(todo_header_label_, "待办");

    for (int i = 0; i < kTodoMaxRows; ++i) {
        todo_row_labels_[i] = lv_label_create(todo_page_);
        lv_obj_set_pos(todo_row_labels_[i], 12, kTodoRowY + i * kTodoRowHeight);
        lv_obj_set_size(todo_row_labels_[i], 376, kTodoRowHeight);
        lv_obj_set_style_text_font(todo_row_labels_[i], &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(todo_row_labels_[i], lv_color_black(), 0);
        lv_label_set_long_mode(todo_row_labels_[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(todo_row_labels_[i], "");
    }

    todo_overflow_label_ = lv_label_create(todo_page_);
    lv_obj_set_pos(todo_overflow_label_, 12, kTodoOverflowY);
    lv_obj_set_size(todo_overflow_label_, 376, 20);
    lv_obj_set_style_text_font(todo_overflow_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(todo_overflow_label_, lv_color_hex(0x72756d), 0);
    lv_label_set_text(todo_overflow_label_, "");
}

void CustomLcdDisplay::UpdateTodoPageInternal() {
    if (!todo_page_ || !todo_header_label_) return;

    auto items = TodoManager::GetInstance().List();
    // 仅未完成 + 按 due_date/due_time 升序（无日期排末尾，按 order）
    std::vector<TodoItem> active;
    active.reserve(items.size());
    for (const auto& it : items) {
        if (!it.completed) active.push_back(it);
    }
    std::sort(active.begin(), active.end(), [](const TodoItem& a, const TodoItem& b) {
        const bool a_empty = a.due_date.empty();
        const bool b_empty = b.due_date.empty();
        if (a_empty && b_empty) return a.order < b.order;
        if (a_empty) return false;
        if (b_empty) return true;
        if (a.due_date != b.due_date) return a.due_date < b.due_date;
        return a.due_time < b.due_time;
    });

    // 头部：'待办 · N 项未完成'，空时显示 '待办 · 暂无未完成'
    char header[64];
    if (active.empty()) {
        snprintf(header, sizeof(header), "待办 · 暂无未完成");
    } else {
        snprintf(header, sizeof(header), "待办 · %d 项未完成", static_cast<int>(active.size()));
    }
    lv_label_set_text(todo_header_label_, header);

    const int shown = std::min<int>(kTodoMaxRows, static_cast<int>(active.size()));
    for (int i = 0; i < kTodoMaxRows; ++i) {
        if (i < shown) {
            const auto& t = active[i];
            // 日期时间前缀：MM-DD 或 MM-DD HH:MM，无日期则空
            char prefix[32] = "";
            if (!t.due_date.empty()) {
                const std::string mmdd = t.due_date.size() >= 10 ? t.due_date.substr(5, 5) : t.due_date;
                snprintf(prefix, sizeof(prefix), "%s%s%s  ", mmdd.c_str(),
                         t.due_time.empty() ? "" : " ", t.due_time.c_str());
            }
            char row[160];
            snprintf(row, sizeof(row), "%s%s", prefix, t.content.c_str());
            lv_label_set_text(todo_row_labels_[i], row);
        } else {
            lv_label_set_text(todo_row_labels_[i], "");
        }
    }

    if (static_cast<int>(active.size()) > kTodoMaxRows) {
        char overflow[32];
        snprintf(overflow, sizeof(overflow), "... 还有 %d 项",
                 static_cast<int>(active.size()) - kTodoMaxRows);
        lv_label_set_text(todo_overflow_label_, overflow);
    } else {
        lv_label_set_text(todo_overflow_label_, "");
    }
}

void CustomLcdDisplay::SwitchToTodoPage() {
    DisplayLockGuard lock(this);
    display_mode_ = MODE_TODO;
    ApplyDisplayMode();
    UpdateTodoPageInternal();
}
