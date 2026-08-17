// REFERENCE_INFO_PAGE_V1
// 设备信息页：5 个分组（系统/内存/网络/电源/运行），黑顶白底。

#include "custom_lcd_display.h"

#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "board.h"
#include "managers/power_save_manager.h"
#include "managers/sensor_manager.h"
#include "managers/sdcard_manager.h"
#include "system_info.h"
#include "wifi_manager.h"

LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);

namespace {
constexpr int kHeaderHeight = 48;
constexpr int kRowStartY = 56;
constexpr int kRowHeight = 24;

void MakePlain(lv_obj_t* object, lv_color_t color) {
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

// SRAM 状态（与后台一致：<20KB 危险，<30KB 紧张，≥30KB 正常）
const char* SramStatus(size_t bytes) {
    if (bytes < 20 * 1024) return "危险";
    if (bytes < 30 * 1024) return "紧张";
    return "正常";
}

// PSRAM 状态（<512KB 危险，<2MB 紧张，≥2MB 充足）
const char* PsramStatus(size_t bytes) {
    if (bytes < 512 * 1024) return "危险";
    if (bytes < 2 * 1024 * 1024) return "紧张";
    return "充足";
}
}  // namespace

void CustomLcdDisplay::SetupInfoUI() {
    DisplayLockGuard lock(this);
    info_page_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(info_page_, 400, 300);
    lv_obj_set_pos(info_page_, 0, 0);
    MakePlain(info_page_, lv_color_white());

    // 顶部黑底白字 header 条
    info_header_strip_ = lv_obj_create(info_page_);
    lv_obj_set_size(info_header_strip_, 400, kHeaderHeight);
    lv_obj_set_pos(info_header_strip_, 0, 0);
    MakePlain(info_header_strip_, lv_color_black());

    info_header_label_ = lv_label_create(info_page_);
    lv_obj_set_pos(info_header_label_, 12, 14);
    lv_obj_set_size(info_header_label_, 376, 24);
    lv_obj_set_style_text_font(info_header_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(info_header_label_, lv_color_white(), 0);
    lv_label_set_text(info_header_label_, "设备信息");

    // 单一大 label 多行文本（省 SRAM——10 个独立 label 会耗尽内部 SRAM）
    info_content_label_ = lv_label_create(info_page_);
    lv_obj_set_pos(info_content_label_, 12, kRowStartY);
    lv_obj_set_size(info_content_label_, 376, 300 - kRowStartY - 8);
    lv_obj_set_style_text_font(info_content_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(info_content_label_, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(info_content_label_, 4, 0);
    lv_label_set_long_mode(info_content_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info_content_label_, "");
}

void CustomLcdDisplay::UpdateInfoPageInternal() {
    if (!info_page_ || !info_header_label_ || !info_content_label_) return;

    char text[1024];
    int offset = 0;

    // 1. 系统
    const char* chip = SystemInfo::GetChipModelName().c_str();
    const int flash_mb = SystemInfo::GetFlashSize() / 1024 / 1024;
    const esp_partition_t* running = esp_ota_get_running_partition();
    offset += snprintf(text + offset, sizeof(text) - offset,
                       "系统  %s · Flash %dMB\n      固件 %s · %s\n\n",
                       chip, flash_mb,
                       esp_app_get_description()->version,
                       running ? running->label : "?");

    // 2. 内存（可用/总量 + 百分比 + 状态）
    // esp_get_free_heap_size() 返回所有 heap（SRAM+PSRAM 混合），不是纯 SRAM。
    // 要用 heap_caps_get_free_size(MALLOC_CAP_INTERNAL) 才是纯 SRAM。
    const size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const int sram_pct = sram_total > 0 ? (sram_free * 100 / sram_total) : 0;
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const int psram_pct = psram_total > 0 ? (psram_free * 100 / psram_total) : 0;
    offset += snprintf(text + offset, sizeof(text) - offset,
                       "内存  SRAM %s %dKB/%dKB (%d%%)\n      PSRAM %s %dKB/%dKB (%d%%)\n\n",
                       SramStatus(sram_free), (int)(sram_free / 1024), (int)(sram_total / 1024), sram_pct,
                       PsramStatus(psram_free), (int)(psram_free / 1024), (int)(psram_total / 1024), psram_pct);

    // 3. 网络
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsConnected()) {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "网络  %s\n      %d dBm · %s\n\n",
                           wifi.GetSsid().c_str(), wifi.GetRssi(), wifi.GetIpAddress().c_str());
    } else {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "网络  未连接\n\n");
    }

    // 4. 电源（电池 + 机内温湿度）
    int battery_level = -1;
    bool charging = false, discharging = false;
    if (Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "电源  电池 %d%%%s\n", battery_level,
                           charging ? " 充电中" : (discharging ? " 放电中" : ""));
    } else {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "电源  电池 未检测\n");
    }
    auto sensor = SensorManager::getInstance().getTempHumidity();
    if (sensor.valid) {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "      机内 %.1f°C · %.0f%% RH\n\n",
                           sensor.temperature, sensor.humidity);
    } else {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "      机内 温湿度未读\n\n");
    }

    // 5. 运行（uptime + 省电模式 + SD 卡）
    const int64_t uptime_sec = esp_timer_get_time() / 1000000LL;
    const int days = static_cast<int>(uptime_sec / 86400);
    const int hours = static_cast<int>((uptime_sec % 86400) / 3600);
    const int mins = static_cast<int>((uptime_sec % 3600) / 60);
    const bool power_save = PowerSaveManager::GetInstance().IsActive();
    if (days > 0) {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "运行  %d天%d小时 · %s\n",
                           days, hours, power_save ? "省电模式" : "正常");
    } else {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "运行  %d小时%d分 · %s\n",
                           hours, mins, power_save ? "省电模式" : "正常");
    }
    uint64_t sd_total = 0, sd_free = 0;
    if (SdcardManager::getInstance().getStats(sd_total, sd_free)) {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "      SD卡 %.1f/%.1fGB",
                           (double)sd_free / 1024 / 1024 / 1024,
                           (double)sd_total / 1024 / 1024 / 1024);
    } else {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "      SD卡 未挂载");
    }

    lv_label_set_text(info_content_label_, text);

    // header 显示分组数
    char header[64];
    snprintf(header, sizeof(header), "设备信息 · 5 组");
    lv_label_set_text(info_header_label_, header);
}

void CustomLcdDisplay::SwitchToInfoPage() {
    DisplayLockGuard lock(this);
    display_mode_ = MODE_INFO;
    ApplyDisplayMode();
    UpdateInfoPageInternal();
}
