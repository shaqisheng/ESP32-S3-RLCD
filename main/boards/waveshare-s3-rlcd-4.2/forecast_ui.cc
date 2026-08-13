// FIXED_COLUMN_FORECAST_V3
// 上方黑色当前天气主视觉，下方七个固定宽度预报列。

#include "custom_lcd_display.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "managers/weather_manager.h"

LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);

LV_IMAGE_DECLARE(ui_img_weather_sunny_large);
LV_IMAGE_DECLARE(ui_img_weather_partly_cloudy_large);
LV_IMAGE_DECLARE(ui_img_weather_overcast_large);
LV_IMAGE_DECLARE(ui_img_weather_rain_large);
LV_IMAGE_DECLARE(ui_img_weather_thunder_large);
LV_IMAGE_DECLARE(ui_img_weather_snow_large);
LV_IMAGE_DECLARE(ui_img_weather_fog_large);
LV_IMAGE_DECLARE(ui_img_weather_unknown_large);
LV_IMAGE_DECLARE(ui_img_weather_sunny_small);
LV_IMAGE_DECLARE(ui_img_weather_partly_cloudy_small);
LV_IMAGE_DECLARE(ui_img_weather_overcast_small);
LV_IMAGE_DECLARE(ui_img_weather_rain_small);
LV_IMAGE_DECLARE(ui_img_weather_thunder_small);
LV_IMAGE_DECLARE(ui_img_weather_snow_small);
LV_IMAGE_DECLARE(ui_img_weather_fog_small);
LV_IMAGE_DECLARE(ui_img_weather_unknown_small);

namespace {
enum class WeatherIconKind : size_t {
    Sunny,
    PartlyCloudy,
    Overcast,
    Rain,
    Thunder,
    Snow,
    Fog,
    Unknown,
};

const lv_image_dsc_t* const kLargeWeatherIcons[] = {
    &ui_img_weather_sunny_large, &ui_img_weather_partly_cloudy_large,
    &ui_img_weather_overcast_large, &ui_img_weather_rain_large,
    &ui_img_weather_thunder_large, &ui_img_weather_snow_large,
    &ui_img_weather_fog_large, &ui_img_weather_unknown_large,
};

const lv_image_dsc_t* const kSmallWeatherIcons[] = {
    &ui_img_weather_sunny_small, &ui_img_weather_partly_cloudy_small,
    &ui_img_weather_overcast_small, &ui_img_weather_rain_small,
    &ui_img_weather_thunder_small, &ui_img_weather_snow_small,
    &ui_img_weather_fog_small, &ui_img_weather_unknown_small,
};

WeatherIconKind WeatherIcon(const std::string& code, const std::string& text) {
    if (!code.empty()) {
        char* end = nullptr;
        const long wmo = strtol(code.c_str(), &end, 10);
        if (end && *end == '\0') {
            if (wmo == 0 || wmo == 100) return WeatherIconKind::Sunny;
            if (wmo == 1 || wmo == 2) return WeatherIconKind::PartlyCloudy;
            if (wmo == 3) return WeatherIconKind::Overcast;
            if (wmo == 45 || wmo == 48) return WeatherIconKind::Fog;
            if ((wmo >= 71 && wmo <= 77) || wmo == 85 || wmo == 86) return WeatherIconKind::Snow;
            if (wmo >= 95 && wmo <= 99) return WeatherIconKind::Thunder;
            if ((wmo >= 51 && wmo <= 67) || (wmo >= 80 && wmo <= 82)) return WeatherIconKind::Rain;
            return WeatherIconKind::Unknown;
        }
    }

    if (text.find("雷") != std::string::npos) return WeatherIconKind::Thunder;
    if (text.find("雪") != std::string::npos) return WeatherIconKind::Snow;
    if (text.find("雾") != std::string::npos || text.find("霾") != std::string::npos ||
        text.find("沙") != std::string::npos || text.find("尘") != std::string::npos) {
        return WeatherIconKind::Fog;
    }
    if (text.find("雨") != std::string::npos) return WeatherIconKind::Rain;
    if (text.find("阴") != std::string::npos) return WeatherIconKind::Overcast;
    if (text.find("多云") != std::string::npos) return WeatherIconKind::PartlyCloudy;
    if (text.find("晴") != std::string::npos) return WeatherIconKind::Sunny;
    return WeatherIconKind::Unknown;
}

const lv_image_dsc_t* LargeWeatherIcon(const std::string& code, const std::string& text) {
    return kLargeWeatherIcons[static_cast<size_t>(WeatherIcon(code, text))];
}

const lv_image_dsc_t* SmallWeatherIcon(const std::string& code, const std::string& text) {
    return kSmallWeatherIcons[static_cast<size_t>(WeatherIcon(code, text))];
}

void MakePlain(lv_obj_t* object, lv_color_t color) {
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}
}  // namespace

void CustomLcdDisplay::SetupForecastUI() {
    DisplayLockGuard lock(this);
    forecast_page_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(forecast_page_, 400, 300);
    lv_obj_set_pos(forecast_page_, 0, 0);
    MakePlain(forecast_page_, lv_color_white());

    lv_obj_t* current_panel = lv_obj_create(forecast_page_);
    lv_obj_set_size(current_panel, 400, 116);
    lv_obj_set_pos(current_panel, 0, 0);
    MakePlain(current_panel, lv_color_black());

    forecast_city_label_ = lv_label_create(forecast_page_);
    lv_obj_set_pos(forecast_city_label_, 10, 8);
    lv_obj_set_width(forecast_city_label_, 230);
    lv_obj_set_style_text_font(forecast_city_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(forecast_city_label_, lv_color_white(), 0);
    lv_label_set_long_mode(forecast_city_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(forecast_city_label_, "七日天气");

    forecast_updated_label_ = lv_label_create(forecast_page_);
    lv_obj_set_pos(forecast_updated_label_, 250, 10);
    lv_obj_set_width(forecast_updated_label_, 140);
    lv_obj_set_style_text_font(forecast_updated_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(forecast_updated_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(forecast_updated_label_, LV_OPA_70, 0);
    lv_obj_set_style_text_align(forecast_updated_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(forecast_updated_label_, "等待更新");

    forecast_now_label_ = lv_label_create(forecast_page_);
    lv_obj_set_pos(forecast_now_label_, 10, 40);
    lv_obj_set_size(forecast_now_label_, 192, 66);
    lv_obj_set_style_text_font(forecast_now_label_, &alibaba_puhui_48, 0);
    lv_obj_set_style_text_color(forecast_now_label_, lv_color_white(), 0);
    lv_label_set_text(forecast_now_label_, "--°");

    forecast_now_icon_image_ = lv_image_create(forecast_page_);
    lv_obj_set_pos(forecast_now_icon_image_, 205, 40);
    lv_image_set_src(forecast_now_icon_image_, &ui_img_weather_unknown_large);
    lv_obj_set_style_image_recolor(forecast_now_icon_image_, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(forecast_now_icon_image_, LV_OPA_COVER, 0);

    forecast_detail_label_ = lv_label_create(forecast_page_);
    lv_obj_set_pos(forecast_detail_label_, 270, 43);
    lv_obj_set_size(forecast_detail_label_, 120, 64);
    lv_obj_set_style_text_font(forecast_detail_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(forecast_detail_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(forecast_detail_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_line_space(forecast_detail_label_, 3, 0);
    lv_label_set_text(forecast_detail_label_, "等待天气\n体感--° · 湿度--%\n风力--");

    for (size_t col = 0; col < forecast_column_labels_.size(); ++col) {
        auto* label = lv_label_create(forecast_page_);
        forecast_column_labels_[col] = label;
        lv_obj_set_pos(label, 4 + static_cast<int>(col) * 56, 128);
        lv_obj_set_size(label, 56, 164);
        lv_obj_set_style_text_font(label, &font_puhui_14_1, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(label, 13, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(label, col == 0 ? "今天\n\n--°\n--°" : "--\n\n--°\n--°");

        auto* icon = lv_image_create(forecast_page_);
        forecast_icon_images_[col] = icon;
        lv_obj_set_pos(icon, 18 + static_cast<int>(col) * 56, 154);
        lv_image_set_src(icon, &ui_img_weather_unknown_small);
        lv_obj_set_style_image_recolor(icon, lv_color_black(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    }
}

void CustomLcdDisplay::UpdateWeatherDisplaysInternal() {
    const auto data = WeatherManager::getInstance().getLatestData();
    if (!data.valid) return;

    if (weather_label_) {
        lv_label_set_text_fmt(weather_label_, "%s°  %s", data.temp.c_str(), data.text.c_str());
    }
    if (weather_icon_image_) {
        lv_image_set_src(weather_icon_image_, LargeWeatherIcon(data.icon, data.text));
    }
    if (weather_detail_label_) {
        lv_label_set_text_fmt(weather_detail_label_, "体感%s°  湿度%s%%\n%s",
                              data.feels_like.c_str(), data.humidity.c_str(), data.wind.c_str());
    }
    if (forecast_city_label_) {
        lv_label_set_text_fmt(forecast_city_label_, "%s · 七日天气", data.city.c_str());
    }
    if (forecast_updated_label_) {
        // BOOT 单击触发刷新时显示"正在刷新…"；完成后回到时间文本
        auto& wm = WeatherManager::getInstance();
        if (wm.IsRefreshing()) {
            lv_label_set_text(forecast_updated_label_, "正在刷新…");
        } else {
            lv_label_set_text(forecast_updated_label_,
                              data.update_time.empty() ? "刚刚更新" : data.update_time.c_str());
        }
    }
    if (forecast_now_label_) lv_label_set_text_fmt(forecast_now_label_, "%s°", data.temp.c_str());
    if (forecast_now_icon_image_) {
        lv_image_set_src(forecast_now_icon_image_, LargeWeatherIcon(data.icon, data.text));
    }
    if (forecast_detail_label_) {
        lv_label_set_text_fmt(forecast_detail_label_, "%s\n体感%s° · 湿度%s%%\n%s",
                              data.text.c_str(), data.feels_like.c_str(),
                              data.humidity.c_str(), data.wind.c_str());
    }
    if (!forecast_column_labels_[0]) return;

    static const char* week[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    for (size_t i = 0; i < forecast_column_labels_.size(); ++i) {
        std::string column = i == 0 ? "今天\n\n--°\n--°" : "--\n\n--°\n--°";
        const lv_image_dsc_t* icon = &ui_img_weather_unknown_small;
        if (i < static_cast<size_t>(data.forecast_count)) {
            const auto& day = data.forecast[i];
            struct tm parsed = {};
            strptime(day.date.c_str(), "%Y-%m-%d", &parsed);
            mktime(&parsed);
            column = i == 0 ? "今天" : week[parsed.tm_wday];
            column += "\n\n" + day.temp_max + "°\n" + day.temp_min + "°";
            icon = SmallWeatherIcon(day.icon, day.text);
        }
        lv_label_set_text(forecast_column_labels_[i], column.c_str());
        if (forecast_icon_images_[i]) lv_image_set_src(forecast_icon_images_[i], icon);
    }
}
