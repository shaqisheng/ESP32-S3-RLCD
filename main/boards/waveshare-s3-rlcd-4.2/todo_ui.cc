// REFERENCE_TODO_LIST_V2
// 顶部黑底白字 header + 白底黑字列表区，空状态显示手绘空白清单图标。
// 见 docs/mockups/device-todo-page-v1.html 的最终设计。

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
constexpr int kHeaderHeight = 48;
constexpr int kRowStartY = 58;
constexpr int kRowHeight = 36;
constexpr int kTodoMaxRows = 5;

// 手绘空白清单图标的三条横线（相对 icon 容器左上角）
// LVGL 9 line API 用 lv_point_precise_t（int32_t x/y）
constexpr lv_point_precise_t kIconLine1[] = {{15, 30}, {75, 30}};
constexpr lv_point_precise_t kIconLine2[] = {{15, 55}, {75, 55}};
constexpr lv_point_precise_t kIconLine3[] = {{15, 80}, {55, 80}};

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

    // 顶部黑底白字 header 条（与 weather/quota/forecast 一致风格）
    // 注意：label 必须创建在 page 上（不能是 strip 的子元素），否则会被
    // strip 容器 clip + 受 pad 影响，导致文字中段被裁。参考 forecast_ui.cc。
    todo_header_strip_ = lv_obj_create(todo_page_);
    lv_obj_set_size(todo_header_strip_, 400, kHeaderHeight);
    lv_obj_set_pos(todo_header_strip_, 0, 0);
    MakePlain(todo_header_strip_, lv_color_black());

    todo_header_label_ = lv_label_create(todo_page_);
    lv_obj_set_pos(todo_header_label_, 12, 8);
    lv_obj_set_size(todo_header_label_, 376, 32);
    lv_obj_set_style_text_font(todo_header_label_, &alibaba_puhui_24, 0);
    lv_obj_set_style_text_color(todo_header_label_, lv_color_white(), 0);
    lv_label_set_text(todo_header_label_, "待办");

    // 5 行待办（白底黑字）
    for (int i = 0; i < kTodoMaxRows; ++i) {
        todo_row_labels_[i] = lv_label_create(todo_page_);
        lv_obj_set_pos(todo_row_labels_[i], 12, kRowStartY + i * kRowHeight);
        lv_obj_set_size(todo_row_labels_[i], 376, kRowHeight);
        lv_obj_set_style_text_font(todo_row_labels_[i], &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(todo_row_labels_[i], lv_color_black(), 0);
        lv_label_set_long_mode(todo_row_labels_[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(todo_row_labels_[i], "");
    }

    todo_overflow_label_ = lv_label_create(todo_page_);
    lv_obj_set_pos(todo_overflow_label_, 12, kRowStartY + kTodoMaxRows * kRowHeight + 6);
    lv_obj_set_size(todo_overflow_label_, 376, 22);
    lv_obj_set_style_text_font(todo_overflow_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(todo_overflow_label_, lv_color_hex(0x72756d), 0);
    lv_label_set_text(todo_overflow_label_, "");
    lv_obj_add_flag(todo_overflow_label_, LV_OBJ_FLAG_HIDDEN);

    // 空状态手绘空白清单图标：方框 + 3 条横线（模拟清单样式）
    // 容器 120x130，居中放在 (140, 100)
    todo_empty_icon_ = lv_obj_create(todo_page_);
    lv_obj_set_size(todo_empty_icon_, 96, 110);
    lv_obj_set_pos(todo_empty_icon_, 152, 95);  // (400-96)/2=152, y=95
    lv_obj_set_style_bg_opa(todo_empty_icon_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(todo_empty_icon_, lv_color_black(), 0);
    lv_obj_set_style_border_width(todo_empty_icon_, 3, 0);
    lv_obj_set_style_radius(todo_empty_icon_, 4, 0);
    lv_obj_set_style_pad_all(todo_empty_icon_, 0, 0);
    lv_obj_remove_flag(todo_empty_icon_, LV_OBJ_FLAG_SCROLLABLE);
    // 三条横线（最后一条短一些，模拟清单末尾）
    for (const auto& pts : {kIconLine1, kIconLine2, kIconLine3}) {
        lv_obj_t* line = lv_line_create(todo_empty_icon_);
        lv_line_set_points(line, pts, 2);
        lv_obj_set_style_line_color(line, lv_color_black(), 0);
        lv_obj_set_style_line_width(line, 4, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
    }
    lv_obj_add_flag(todo_empty_icon_, LV_OBJ_FLAG_HIDDEN);

    // 空状态文字（用黑色而不是灰，白底上对比度足够）
    todo_empty_text_ = lv_label_create(todo_page_);
    lv_obj_set_pos(todo_empty_text_, 0, 230);
    lv_obj_set_size(todo_empty_text_, 400, 30);
    lv_obj_set_style_text_font(todo_empty_text_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(todo_empty_text_, lv_color_black(), 0);
    lv_obj_set_style_text_align(todo_empty_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(todo_empty_text_, "暂无待办");
    lv_obj_add_flag(todo_empty_text_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::UpdateTodoPageInternal() {
    if (!todo_page_ || !todo_header_label_) return;

    auto items = TodoManager::GetInstance().List();
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

    // 头部
    char header[64];
    if (active.empty()) {
        snprintf(header, sizeof(header), "待办 · 暂无未完成");
    } else {
        snprintf(header, sizeof(header), "待办 · %d 项未完成", static_cast<int>(active.size()));
    }
    lv_label_set_text(todo_header_label_, header);

    // 空状态切换
    if (active.empty()) {
        lv_obj_remove_flag(todo_empty_icon_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(todo_empty_text_, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < kTodoMaxRows; ++i) {
            lv_obj_add_flag(todo_row_labels_[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(todo_overflow_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(todo_empty_icon_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(todo_empty_text_, LV_OBJ_FLAG_HIDDEN);

    // 填充行
    const int shown = std::min<int>(kTodoMaxRows, static_cast<int>(active.size()));
    for (int i = 0; i < kTodoMaxRows; ++i) {
        if (i < shown) {
            lv_obj_remove_flag(todo_row_labels_[i], LV_OBJ_FLAG_HIDDEN);
            const auto& t = active[i];
            char prefix[32] = "";
            if (!t.due_date.empty()) {
                const std::string mmdd = t.due_date.size() >= 10 ? t.due_date.substr(5, 5) : t.due_date;
                snprintf(prefix, sizeof(prefix), "%s%s%s  ", mmdd.c_str(),
                         t.due_time.empty() ? "" : " ", t.due_time.c_str());
            }
            char row[160];
            snprintf(row, sizeof(row), "□ %s%s", prefix, t.content.c_str());
            lv_label_set_text(todo_row_labels_[i], row);
        } else {
            lv_label_set_text(todo_row_labels_[i], "");
            lv_obj_remove_flag(todo_row_labels_[i], LV_OBJ_FLAG_HIDDEN);  // 留白可显示空 label
        }
    }

    if (static_cast<int>(active.size()) > kTodoMaxRows) {
        char overflow[32];
        snprintf(overflow, sizeof(overflow), "... 还有 %d 项",
                 static_cast<int>(active.size()) - kTodoMaxRows);
        lv_label_set_text(todo_overflow_label_, overflow);
        lv_obj_remove_flag(todo_overflow_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(todo_overflow_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CustomLcdDisplay::SwitchToTodoPage() {
    DisplayLockGuard lock(this);
    display_mode_ = MODE_TODO;
    ApplyDisplayMode();
    UpdateTodoPageInternal();
}
