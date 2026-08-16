#include "power_save_manager.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <nvs_flash.h>
#include <nvs.h>

#include <cstring>
#include <ctime>

#include "quota_manager.h"
#include "weather_manager.h"
#include "board.h"
#include "application.h"
#include "calendar_manager.h"

namespace {
constexpr const char* TAG = "PowerSaveMgr";
constexpr const char* kPartition = "quota_nvs";
constexpr const char* kNamespace = "power_save";

// cJSON 辅助（与 quota_manager 等保持一致的宽松解析）
std::string JsonString(cJSON* obj, const char* key, const char* fallback = "") {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : fallback;
}
double JsonNumber(cJSON* obj, const char* key, double fallback = 0) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(value)) return value->valuedouble;
    if (cJSON_IsString(value) && value->valuestring) return atof(value->valuestring);
    return fallback;
}
bool JsonBool(cJSON* obj, const char* key, bool fallback) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(value) ? cJSON_IsTrue(value) : fallback;
}
}  // namespace

void PowerSaveManager::Init() {
    LoadFromNvs();
    ESP_LOGI(TAG, "省电模式管理器初始化完成（manual=%d, 阈值=%d%%, 夜间=%02d:00-%02d:00 enabled=%d）",
             manual_override_.load(), battery_threshold_.load(),
             night_start_hour_.load(), night_end_hour_.load(), night_enabled_.load());
}

std::string PowerSaveManager::GetReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reason_;
}

void PowerSaveManager::SetActive(bool active, const char* reason) {
    const bool was_active = active_.load();
    if (was_active == active) return;
    active_.store(active);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reason_ = active ? reason : "";
    }
    if (active) {
        enter_at_.store(time(nullptr));
        ESP_LOGI(TAG, "进入省电模式：%s", reason);
        // 1. 拉长 Quota 刷新间隔到 60 分钟（只改运行时，不写 NVS——
        // 避免覆盖用户在后台配置的刷新间隔，退出后从 NVS 恢复）
        auto& qm = QuotaManager::GetInstance();
        auto& wm = WeatherManager::getInstance();
        orig_quota_interval_minutes_ = qm.GetRefreshIntervalMinutes();
        orig_weather_interval_minutes_ = wm.GetRefreshIntervalMinutes();
        qm.SetRefreshIntervalMinutesRuntime(kPowerSaveRefreshMinutes);
        ESP_LOGI(TAG, "刷新间隔已拉长到 %d 分钟（原 quota=%d, weather=%d）",
                 (int)kPowerSaveRefreshMinutes,
                 (int)orig_quota_interval_minutes_,
                 (int)orig_weather_interval_minutes_);
        // 2. Wi-Fi 降到最大省电（DTIM 延长）
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        ESP_LOGI(TAG, "Wi-Fi 省电已启用（LOW_POWER / MAX_MODEM）");
        // 3. CPU 降频到 80MHz
        esp_pm_config_esp32s3_t pm_cfg;
        pm_cfg.max_freq_mhz = 80;
        pm_cfg.min_freq_mhz = 80;
        pm_cfg.light_sleep_enable = false;
        if (esp_pm_configure(&pm_cfg) == ESP_OK) {
            ESP_LOGI(TAG, "CPU 已降频到 80MHz");
        } else {
            ESP_LOGW(TAG, "CPU 降频失败（可能 esp_pm 未启用）");
        }
        // 4. 关闭 AFE 语音唤醒（只保留 BOOT 按钮手动启动对话）
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
        ESP_LOGI(TAG, "AFE 语音唤醒已关闭");
    } else {
        enter_at_.store(0);
        ESP_LOGI(TAG, "退出省电模式");
        // 恢复 Quota 刷新间隔（用运行时 setter 恢复原值，不动 NVS）
        if (orig_quota_interval_minutes_ > 0) {
            QuotaManager::GetInstance().SetRefreshIntervalMinutesRuntime(orig_quota_interval_minutes_);
            ESP_LOGI(TAG, "刷新间隔已恢复 quota=%d 分钟", (int)orig_quota_interval_minutes_);
        }
        // 恢复 Wi-Fi PS（用 BALANCED 而不是 LOW_POWER，平衡性能）
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::BALANCED);
        ESP_LOGI(TAG, "Wi-Fi 省电已恢复（BALANCED）");
        // 恢复 CPU 频率到 240MHz
        esp_pm_config_esp32s3_t pm_cfg;
        pm_cfg.max_freq_mhz = 240;
        pm_cfg.min_freq_mhz = 240;
        pm_cfg.light_sleep_enable = false;
        if (esp_pm_configure(&pm_cfg) == ESP_OK) {
            ESP_LOGI(TAG, "CPU 已恢复到 240MHz");
        }
        // 恢复 AFE 语音唤醒
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
        ESP_LOGI(TAG, "AFE 语音唤醒已恢复");
    }
}

void PowerSaveManager::Evaluate(int battery_level, bool charging, int current_hour) {
    // 退出条件：只要充电就退出
    if (charging) {
        SetActive(false, "");
        return;
    }

    // 手动临时退出：今天 23:59:59 前无视所有自动条件（低电量/夜间/休息日）
    const int64_t now = time(nullptr);
    if (manual_exit_until_.load() > 0 && now < manual_exit_until_.load()) {
        SetActive(false, "");
        return;
    }

    // 手动开关优先级最高：手动开启时强制启用（无视其他条件），
    // 手动关闭时强制退出（无视低电量/夜间/休息日）。
    // 这样用户能随时用后台一键切换，不会被自动条件顶回。
    if (manual_override_.load()) {
        SetActive(true, "手动");
        return;
    }

    // 进入条件（手动关闭时的自动触发）
    const bool low_battery = battery_level >= 0 && battery_level < battery_threshold_.load();

    // 法定节假日/周末/调休日全天省电（rest_day_all_day_ 启用时）
    bool rest_day = false;
    if (rest_day_all_day_.load()) {
        time_t now_t = time(nullptr);
        struct tm now_tm;
        localtime_r(&now_t, &now_tm);
        // 周末（周六=6, 周日=0）算休息日
        if (now_tm.tm_wday == 0 || now_tm.tm_wday == 6) {
            rest_day = true;
        } else {
            // 法定节假日（off_day=true）算休息日
            // 调休上班（off_day=false）算工作日，不算休息日
            char date_str[32];
            snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d",
                     now_tm.tm_year + 1900, now_tm.tm_mon + 1, now_tm.tm_mday);
            auto holiday = CalendarManager::GetInstance().Find(date_str);
            if (!holiday.date.empty() && holiday.off_day) {
                rest_day = true;
            }
        }
    }

    // 夜间时段（rest_day 时无视夜间时段直接全天启用）
    bool night = false;
    if (night_enabled_.load() && !rest_day) {
        const uint8_t start = night_start_hour_.load();
        const uint8_t end = night_end_hour_.load();
        if (start < end) {
            night = current_hour >= start && current_hour < end;
        } else if (start > end) {
            // 跨天：如 23-7 表示 23:00 到次日 07:00
            night = current_hour >= start || current_hour < end;
        }
    }

    if (low_battery) SetActive(true, "低电量");
    else if (rest_day) SetActive(true, "休息日全天");
    else if (night) SetActive(true, "夜间时段");
    else SetActive(false, "");
}

bool PowerSaveManager::SetManualOverride(bool enabled, std::string& error) {
    manual_override_.store(enabled);
    if (enabled) {
        // 手动开启：清除临时退出标记（允许自动条件）
        manual_exit_until_.store(0);
    } else {
        // 手动关闭：设置临时退出截止到今天 23:59:59，今天不再自动进入
        time_t now_t = time(nullptr);
        struct tm now_tm;
        localtime_r(&now_t, &now_tm);
        now_tm.tm_hour = 23;
        now_tm.tm_min = 59;
        now_tm.tm_sec = 59;
        manual_exit_until_.store(mktime(&now_tm));
    }
    SaveToNvs();
    return true;
}

bool PowerSaveManager::SetBatteryThreshold(uint8_t percent, std::string& error) {
    if (percent < 5 || percent > 50) {
        error = "电池阈值必须为 5-50%";
        return false;
    }
    battery_threshold_.store(percent);
    SaveToNvs();
    return true;
}

bool PowerSaveManager::SetNightWindow(uint8_t start_hour, uint8_t end_hour, bool enabled, std::string& error) {
    if (start_hour > 23 || end_hour > 23) {
        error = "时段必须为 0-23";
        return false;
    }
    night_start_hour_.store(start_hour);
    night_end_hour_.store(end_hour);
    night_enabled_.store(enabled);
    SaveToNvs();
    return true;
}

bool PowerSaveManager::SetRestDayAllDay(bool all_day, std::string& error) {
    rest_day_all_day_.store(all_day);
    SaveToNvs();
    return true;
}

void PowerSaveManager::LoadFromNvs() {
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t u8 = 0;
    if (nvs_get_u8(handle, "manual", &u8) == ESP_OK) manual_override_.store(u8 != 0);
    if (nvs_get_u8(handle, "threshold", &u8) == ESP_OK && u8 >= 5 && u8 <= 50) battery_threshold_.store(u8);
    if (nvs_get_u8(handle, "night_start", &u8) == ESP_OK && u8 <= 23) night_start_hour_.store(u8);
    if (nvs_get_u8(handle, "night_end", &u8) == ESP_OK && u8 <= 23) night_end_hour_.store(u8);
    if (nvs_get_u8(handle, "night_on", &u8) == ESP_OK) night_enabled_.store(u8 != 0);
    if (nvs_get_u8(handle, "rest_day", &u8) == ESP_OK) rest_day_all_day_.store(u8 != 0);
    nvs_close(handle);
}

void PowerSaveManager::SaveToNvs() {
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "manual", manual_override_.load() ? 1 : 0);
    nvs_set_u8(handle, "threshold", battery_threshold_.load());
    nvs_set_u8(handle, "night_start", night_start_hour_.load());
    nvs_set_u8(handle, "night_end", night_end_hour_.load());
    nvs_set_u8(handle, "night_on", night_enabled_.load() ? 1 : 0);
    nvs_set_u8(handle, "rest_day", rest_day_all_day_.load() ? 1 : 0);
    nvs_commit(handle);
    nvs_close(handle);
}

std::string PowerSaveManager::GetStatusJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", active_.load());
    cJSON_AddStringToObject(root, "reason", GetReason().c_str());
    cJSON_AddNumberToObject(root, "enter_at", static_cast<double>(enter_at_.load()));
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return out;
}

std::string PowerSaveManager::GetConfigJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "manual_override", manual_override_.load());
    cJSON_AddNumberToObject(root, "battery_threshold", battery_threshold_.load());
    cJSON_AddNumberToObject(root, "night_start_hour", night_start_hour_.load());
    cJSON_AddNumberToObject(root, "night_end_hour", night_end_hour_.load());
    cJSON_AddBoolToObject(root, "night_enabled", night_enabled_.load());
    cJSON_AddBoolToObject(root, "rest_day_all_day", rest_day_all_day_.load());
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return out;
}

bool PowerSaveManager::ApplyConfigJson(const char* json, std::string& error) {
    cJSON* root = cJSON_Parse(json);
    if (!root) { error = "JSON 解析失败"; return false; }
    const bool manual = JsonBool(root, "manual_override", manual_override_.load());
    const int threshold = static_cast<int>(JsonNumber(root, "battery_threshold", battery_threshold_.load()));
    const int start = static_cast<int>(JsonNumber(root, "night_start_hour", night_start_hour_.load()));
    const int end = static_cast<int>(JsonNumber(root, "night_end_hour", night_end_hour_.load()));
    const bool night_on = JsonBool(root, "night_enabled", night_enabled_.load());
    const bool rest_day = JsonBool(root, "rest_day_all_day", rest_day_all_day_.load());
    cJSON_Delete(root);
    if (threshold < 5 || threshold > 50) { error = "电池阈值必须为 5-50%"; return false; }
    if (start < 0 || start > 23 || end < 0 || end > 23) { error = "时段必须为 0-23"; return false; }
    manual_override_.store(manual);
    if (manual) {
        // 手动开启：清除临时退出标记
        manual_exit_until_.store(0);
    } else {
        // 手动关闭：设置临时退出截止到今天 23:59:59
        time_t now_t = time(nullptr);
        struct tm now_tm;
        localtime_r(&now_t, &now_tm);
        now_tm.tm_hour = 23;
        now_tm.tm_min = 59;
        now_tm.tm_sec = 59;
        manual_exit_until_.store(mktime(&now_tm));
    }
    battery_threshold_.store(static_cast<uint8_t>(threshold));
    night_start_hour_.store(static_cast<uint8_t>(start));
    night_end_hour_.store(static_cast<uint8_t>(end));
    night_enabled_.store(night_on);
    rest_day_all_day_.store(rest_day);
    SaveToNvs();
    return true;
}
