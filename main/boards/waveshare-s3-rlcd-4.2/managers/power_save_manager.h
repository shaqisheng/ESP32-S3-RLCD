#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// 省电模式管理器（单例）
//
// 触发条件（任一满足即进入）：
//   1. 电池 < 阈值（默认 20%）且未充电
//   2. 后台手动开关打开
//   3. 当前时间在夜间时段（默认 23:00-07:00）
//
// 退出条件：只要充电就退出（最简单，避免滞回复杂度）
//
// Phase 1 只提供状态查询和配置存储，不改变任何系统行为。
// Phase 2 起才会真正降低刷新频率/Wi-Fi/CPU/AFE。
class PowerSaveManager {
public:
    static PowerSaveManager& GetInstance() {
        static PowerSaveManager instance;
        return instance;
    }

    void Init();

    // 当前是否处于省电模式
    bool IsActive() const { return active_.load(); }

    // 进入原因（"低电量"/"手动"/"夜间时段"，未启用时空字符串）
    std::string GetReason() const;

    // 进入时间（epoch 秒，未启用时 0）
    int64_t GetEnterAt() const { return enter_at_.load(); }

    // ===== 配置（NVS 持久化）=====
    bool GetManualOverride() const { return manual_override_.load(); }
    uint8_t GetBatteryThreshold() const { return battery_threshold_.load(); }
    uint8_t GetNightStartHour() const { return night_start_hour_.load(); }
    uint8_t GetNightEndHour() const { return night_end_hour_.load(); }
    bool GetNightEnabled() const { return night_enabled_.load(); }
    // 全天省电（法定节假日/周末/调休日）：这些日子无视夜间时段直接 24h 启用
    bool GetRestDayAllDay() const { return rest_day_all_day_.load(); }

    // 省电模式下的刷新间隔（分钟）——进入时 Quota/Weather 都用这个
    static constexpr uint32_t kPowerSaveRefreshMinutes = 60;

    bool SetManualOverride(bool enabled, std::string& error);
    bool SetBatteryThreshold(uint8_t percent, std::string& error);
    bool SetNightWindow(uint8_t start_hour, uint8_t end_hour, bool enabled, std::string& error);
    bool SetRestDayAllDay(bool all_day, std::string& error);

    // 由 data_update_task 每秒调用：根据当前条件评估是否进入/退出
    void Evaluate(int battery_level, bool charging, int current_hour);

    // JSON 供后台 API
    std::string GetStatusJson() const;
    std::string GetConfigJson() const;
    bool ApplyConfigJson(const char* json, std::string& error);

private:
    PowerSaveManager() = default;

    std::atomic<bool> active_{false};
    std::atomic<int64_t> enter_at_{0};
    std::string reason_;  // 只在 active_ 变化时更新，用 mutex_

    // 配置（NVS 持久化到 power_save namespace）
    std::atomic<bool> manual_override_{false};
    std::atomic<uint8_t> battery_threshold_{20};
    std::atomic<uint8_t> night_start_hour_{23};
    std::atomic<uint8_t> night_end_hour_{7};
    std::atomic<bool> night_enabled_{false};
    std::atomic<bool> rest_day_all_day_{false};  // true=节假日/周末/调休日全天启用省电

    // 原配置备份（退出省电模式时恢复）—— Phase 2 使用
    uint32_t orig_quota_interval_minutes_ = 0;
    uint32_t orig_weather_interval_minutes_ = 0;

    mutable std::mutex mutex_;  // 保护 reason_ 和 NVS 写

    void LoadFromNvs();
    void SaveToNvs();
    void SetActive(bool active, const char* reason);
};
