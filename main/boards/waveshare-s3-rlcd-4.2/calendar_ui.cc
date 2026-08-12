// REFERENCE_MONTH_CALENDAR_V5
// 参考纸质月历：顶部月份信息、黑色星期栏、42 个独立日期格。

#include "custom_lcd_display.h"

#include <cstdio>
#include <ctime>
#include <string>

#include "managers/calendar_manager.h"

LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);

namespace {
constexpr int kCellWidth = 56;
constexpr int kGridY = 91;
constexpr int kCellHeight = 34;
constexpr const char* kWeekdays[] = {"一", "二", "三", "四", "五", "六", "日"};

void MakePlain(lv_obj_t* object, lv_color_t color) {
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}
}  // namespace

void CustomLcdDisplay::SetupCalendarUI() {
    DisplayLockGuard lock(this);
    calendar_page_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(calendar_page_, 400, 300);
    lv_obj_set_pos(calendar_page_, 0, 0);
    MakePlain(calendar_page_, lv_color_white());

    calendar_year_label_ = lv_label_create(calendar_page_);
    lv_obj_set_pos(calendar_year_label_, 12, 6);
    lv_obj_set_size(calendar_year_label_, 154, 47);
    lv_obj_set_style_text_font(calendar_year_label_, &alibaba_puhui_48, 0);
    lv_obj_set_style_text_color(calendar_year_label_, lv_color_black(), 0);
    lv_label_set_long_mode(calendar_year_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(calendar_year_label_, "----年");

    calendar_month_label_ = lv_label_create(calendar_page_);
    lv_obj_set_pos(calendar_month_label_, 316, 6);
    lv_obj_set_size(calendar_month_label_, 72, 30);
    lv_obj_set_style_text_font(calendar_month_label_, &alibaba_puhui_24, 0);
    lv_obj_set_style_text_color(calendar_month_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(calendar_month_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(calendar_month_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(calendar_month_label_, "--月");

    calendar_subtitle_label_ = lv_label_create(calendar_page_);
    lv_obj_set_pos(calendar_subtitle_label_, 236, 38);
    lv_obj_set_size(calendar_subtitle_label_, 152, 20);
    lv_obj_set_style_text_font(calendar_subtitle_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(calendar_subtitle_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(calendar_subtitle_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(calendar_subtitle_label_, "农历 · 等待同步");

    lv_obj_t* weekday_bar = lv_obj_create(calendar_page_);
    lv_obj_set_size(weekday_bar, 392, 27);
    lv_obj_set_pos(weekday_bar, 4, 59);
    MakePlain(weekday_bar, lv_color_black());
    for (size_t col = 0; col < calendar_column_labels_.size(); ++col) {
        auto* label = lv_label_create(weekday_bar);
        calendar_column_labels_[col] = label;
        lv_obj_set_pos(label, static_cast<int>(col) * kCellWidth, 4);
        lv_obj_set_size(label, kCellWidth, 20);
        lv_obj_set_style_text_font(label, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(label, kWeekdays[col]);
    }

    for (int cell_index = 0; cell_index < 42; ++cell_index) {
        auto& cell = calendar_day_cells_[cell_index];
        const int row = cell_index / 7;
        const int col = cell_index % 7;
        cell.root = lv_obj_create(calendar_page_);
        lv_obj_set_pos(cell.root, 4 + col * kCellWidth, kGridY + row * kCellHeight);
        lv_obj_set_size(cell.root, kCellWidth, kCellHeight);
        MakePlain(cell.root, lv_color_white());
        lv_obj_set_style_radius(cell.root, 17, 0);

        cell.day = lv_label_create(cell.root);
        lv_obj_set_pos(cell.day, 0, 1);
        lv_obj_set_size(cell.day, kCellWidth, 17);
        lv_obj_set_style_text_font(cell.day, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(cell.day, lv_color_black(), 0);
        lv_obj_set_style_text_align(cell.day, LV_TEXT_ALIGN_CENTER, 0);

        cell.detail = lv_label_create(cell.root);
        lv_obj_set_pos(cell.detail, 0, 18);
        lv_obj_set_size(cell.detail, kCellWidth, 15);
        lv_obj_set_style_text_font(cell.detail, &font_puhui_14_1, 0);
        lv_obj_set_style_text_color(cell.detail, lv_color_black(), 0);
        lv_obj_set_style_text_align(cell.detail, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(cell.detail, LV_LABEL_LONG_CLIP);
    }
}

void CustomLcdDisplay::UpdateCalendarInternal(const struct tm& now) {
    if (!calendar_year_label_ || !calendar_month_label_ || !calendar_day_cells_[0].root) return;
    const int year = now.tm_year + 1900;
    const int month = now.tm_mon + 1;
    char year_title[20];
    snprintf(year_title, sizeof(year_title), "%d年", year);
    lv_label_set_text(calendar_year_label_, year_title);
    char month_title[20];
    snprintf(month_title, sizeof(month_title), "%d月", month);
    lv_label_set_text(calendar_month_label_, month_title);

    char subtitle[96];
    snprintf(subtitle, sizeof(subtitle), "农历 %s",
             CalendarManager::LunarFullText(year, month, now.tm_mday).c_str());
    lv_label_set_text(calendar_subtitle_label_, subtitle);

    struct tm first = now;
    first.tm_mday = 1;
    first.tm_hour = 12;
    mktime(&first);
    const int offset = (first.tm_wday + 6) % 7;
    for (int cell_index = 0; cell_index < 42; ++cell_index) {
        auto& cell = calendar_day_cells_[cell_index];
        struct tm date = first;
        date.tm_mday = 1 + cell_index - offset;
        mktime(&date);
        const int date_year = date.tm_year + 1900;
        const int date_month = date.tm_mon + 1;
        const int date_day = date.tm_mday;
        const bool current_month = date_year == year && date_month == month;
        const bool today = current_month && date_day == now.tm_mday;
        const bool visible = current_month;
        if (visible) lv_obj_clear_flag(cell.root, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(cell.root, LV_OBJ_FLAG_HIDDEN);
        if (!visible) continue;

        const lv_color_t foreground = today ? lv_color_white() : lv_color_black();
        lv_obj_set_style_bg_color(cell.root, today ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cell.root, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cell.root, today ? 17 : 0, 0);
        lv_obj_set_style_text_color(cell.day, foreground, 0);
        lv_obj_set_style_text_color(cell.detail, foreground, 0);
        char number[8];
        snprintf(number, sizeof(number), "%d", date_day);
        lv_label_set_text(cell.day, number);
        char iso[40];
        snprintf(iso, sizeof(iso), "%04d-%02d-%02d", date_year, date_month, date_day);
        const auto holiday = CalendarManager::GetInstance().Find(iso);
        std::string detail;
        if (!holiday.date.empty()) {
            detail = holiday.off_day ? "休" : "班";
        } else {
            detail = CalendarManager::TraditionalDayText(date_year, date_month, date_day);
            if (detail.empty()) detail = CalendarManager::LunarText(date_year, date_month, date_day);
        }
        lv_label_set_text(cell.detail, detail.c_str());
    }
}
