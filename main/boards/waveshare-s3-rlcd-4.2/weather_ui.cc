// EDITORIAL_OVERVIEW_V3
// 400×300 单色编辑式主页：上方黑色信息区，下方小智与待办白色工作区。

#include "custom_lcd_display.h"

#include <esp_log.h>

LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(alibaba_black_64);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);

LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);
LV_IMAGE_DECLARE(ui_img_weather_unknown_large);

namespace {
constexpr int kTopHeight = 184;
constexpr int kAssistantWidth = 240;
constexpr int kEmotionWidth = 76;
const char* TAG = "WeatherUI";

void MakePlainPanel(lv_obj_t* object, lv_color_t color) {
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

void MakeHiddenLabel(lv_obj_t*& label, lv_obj_t* parent) {
    label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
}
}  // namespace

void CustomLcdDisplay::SetupWeatherUI() {
    DisplayLockGuard lock(this);
    lv_obj_t* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);

    weather_page_ = lv_obj_create(root);
    lv_obj_set_size(weather_page_, 400, 300);
    lv_obj_set_pos(weather_page_, 0, 0);
    MakePlainPanel(weather_page_, lv_color_black());

    sensor_label_ = lv_label_create(weather_page_);
    lv_obj_set_pos(sensor_label_, 10, 7);
    lv_obj_set_style_text_font(sensor_label_, &alibaba_puhui_16, 0);
    lv_obj_set_style_text_color(sensor_label_, lv_color_white(), 0);
    lv_label_set_text(sensor_label_, "--.-°C · --% RH");

    lv_obj_t* status_strip = lv_obj_create(weather_page_);
    lv_obj_set_size(status_strip, 112, 27);
    lv_obj_set_pos(status_strip, 280, 4);
    MakePlainPanel(status_strip, lv_color_black());
    lv_obj_set_style_pad_left(status_strip, 8, 0);
    lv_obj_set_style_pad_right(status_strip, 8, 0);
    lv_obj_set_flex_flow(status_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_strip, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_strip, 5, 0);

    // RGB565 资源自带白色背景，黑底上强制重着色会变成白色方块。
    overview_wifi_symbol_ = lv_label_create(status_strip);
    lv_label_set_text(overview_wifi_symbol_, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(overview_wifi_symbol_, lv_color_white(), 0);
    lv_obj_set_style_text_font(overview_wifi_symbol_, LV_FONT_DEFAULT, 0);
    overview_battery_symbol_ = lv_label_create(status_strip);
    lv_label_set_text(overview_battery_symbol_, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(overview_battery_symbol_, lv_color_white(), 0);
    lv_obj_set_style_text_font(overview_battery_symbol_, LV_FONT_DEFAULT, 0);
    battery_pct_label_ = lv_label_create(status_strip);
    lv_obj_set_style_text_font(battery_pct_label_, &alibaba_puhui_16, 0);
    lv_obj_set_style_text_color(battery_pct_label_, lv_color_white(), 0);
    lv_label_set_text(battery_pct_label_, "---%");

    time_label_ = lv_label_create(weather_page_);
    lv_obj_set_pos(time_label_, 8, 36);
    lv_obj_set_style_text_font(time_label_, &alibaba_black_64, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(time_label_, 1, 0);
    lv_label_set_text(time_label_, "00:00");

    date_detail_label_ = lv_label_create(weather_page_);
    lv_obj_set_pos(date_detail_label_, 12, 106);
    lv_obj_set_size(date_detail_label_, 238, 42);
    lv_obj_set_style_text_font(date_detail_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(date_detail_label_, lv_color_white(), 0);
    lv_label_set_long_mode(date_detail_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(date_detail_label_, 2, 0);
    lv_label_set_text(date_detail_label_, "年月日 · 周一\n农历");

    weather_icon_image_ = lv_image_create(weather_page_);
    lv_obj_set_pos(weather_icon_image_, 328, 36);
    lv_image_set_src(weather_icon_image_, &ui_img_weather_unknown_large);
    lv_obj_set_style_image_recolor(weather_icon_image_, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(weather_icon_image_, LV_OPA_COVER, 0);

    weather_label_ = lv_label_create(weather_page_);
    lv_obj_set_pos(weather_label_, 272, 93);
    lv_obj_set_width(weather_label_, 116);
    lv_obj_set_style_text_font(weather_label_, &alibaba_puhui_24, 0);
    lv_obj_set_style_text_color(weather_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(weather_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(weather_label_, "--° 等待天气");

    weather_detail_label_ = lv_label_create(weather_page_);
    lv_obj_set_pos(weather_detail_label_, 258, 126);
    lv_obj_set_size(weather_detail_label_, 130, 50);
    lv_obj_set_style_text_font(weather_detail_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(weather_detail_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(weather_detail_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_line_space(weather_detail_label_, 2, 0);
    lv_label_set_text(weather_detail_label_, "体感--°  湿度--%\n风力--");

    // 旧的星期和日期成员仍由 DataUpdateTask 更新，保留为隐藏占位，避免改变公共更新链路。
    MakeHiddenLabel(day_label_, weather_page_);
    MakeHiddenLabel(date_num_label_, weather_page_);

    chat_card_ = lv_obj_create(weather_page_);
    lv_obj_set_pos(chat_card_, 0, kTopHeight);
    lv_obj_set_size(chat_card_, kAssistantWidth, 300 - kTopHeight);
    MakePlainPanel(chat_card_, lv_color_white());

    emotion_img_ = lv_image_create(chat_card_);
    lv_obj_set_size(emotion_img_, 48, 48);
    lv_image_set_inner_align(emotion_img_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_pos(emotion_img_, 14, 12);
    lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);

    emotion_label_ = lv_label_create(chat_card_);
    lv_obj_set_pos(emotion_label_, 5, 75);
    lv_obj_set_width(emotion_label_, 66);
    lv_obj_set_style_text_font(emotion_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(emotion_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(emotion_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(emotion_label_, "待命");

    lv_obj_t* assistant_divider = lv_obj_create(chat_card_);
    lv_obj_set_pos(assistant_divider, kEmotionWidth, 10);
    lv_obj_set_size(assistant_divider, 1, 96);
    MakePlainPanel(assistant_divider, lv_color_black());

    chat_status_label_ = lv_label_create(chat_card_);
    lv_obj_set_width(chat_status_label_, 146);
    lv_obj_set_style_text_font(chat_status_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(chat_status_label_, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(chat_status_label_, 3, 0);
    lv_label_set_long_mode(chat_status_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(chat_status_label_, "小智 · 待命\n有什么可以帮你？");
    lv_obj_align(chat_status_label_, LV_ALIGN_LEFT_MID, 84, 0);

    lv_obj_t* todo_panel = lv_obj_create(weather_page_);
    lv_obj_set_pos(todo_panel, kAssistantWidth, kTopHeight);
    lv_obj_set_size(todo_panel, 400 - kAssistantWidth, 300 - kTopHeight);
    MakePlainPanel(todo_panel, lv_color_white());
    lv_obj_set_style_border_side(todo_panel, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(todo_panel, 1, 0);
    lv_obj_set_style_border_color(todo_panel, lv_color_black(), 0);

    lv_obj_t* todo_title = lv_label_create(todo_panel);
    lv_obj_set_pos(todo_title, 10, 8);
    lv_obj_set_style_text_font(todo_title, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(todo_title, lv_color_black(), 0);
    lv_label_set_text(todo_title, "今日待办");

    lv_obj_t* todo_rule = lv_obj_create(todo_panel);
    lv_obj_set_pos(todo_rule, 10, 31);
    lv_obj_set_size(todo_rule, 140, 1);
    MakePlainPanel(todo_rule, lv_color_black());

    memo_list_label_ = lv_label_create(todo_panel);
    lv_obj_set_pos(memo_list_label_, 10, 38);
    lv_obj_set_size(memo_list_label_, 140, 70);
    lv_obj_set_style_text_font(memo_list_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(memo_list_label_, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(memo_list_label_, 3, 0);
    lv_label_set_long_mode(memo_list_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(memo_list_label_, "暂无待办");

    // LcdDisplay 基类仍会访问这些成员，使用隐藏对象保证兼容。
    container_ = lv_obj_create(weather_page_);
    lv_obj_set_size(container_, 1, 1);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    MakeHiddenLabel(network_label_, weather_page_);
    MakeHiddenLabel(battery_label_, weather_page_);
    MakeHiddenLabel(status_label_, weather_page_);
    MakeHiddenLabel(notification_label_, weather_page_);
    MakeHiddenLabel(mute_label_, weather_page_);

    low_battery_popup_ = lv_obj_create(weather_page_);
    lv_obj_set_size(low_battery_popup_, 320, 42);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(low_battery_popup_, 2, 0);
    lv_obj_set_style_border_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 0, 0);
    lv_obj_set_style_pad_all(low_battery_popup_, 6, 0);
    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_obj_set_style_text_font(low_battery_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_black(), 0);
    lv_obj_set_width(low_battery_label_, 300);
    lv_obj_set_style_text_align(low_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(low_battery_label_);
    lv_label_set_text(low_battery_label_, "电量低，请尽快充电");

    emoji_label_ = lv_label_create(weather_page_);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    emoji_image_ = lv_img_create(weather_page_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    chat_message_label_ = chat_status_label_;

    ESP_LOGI(TAG, "编辑式主页 UI 创建完成");
}
