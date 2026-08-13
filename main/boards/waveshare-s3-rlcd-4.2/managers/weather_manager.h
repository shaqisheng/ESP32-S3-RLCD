#pragma once
#include <string>
#include <array>
#include <atomic>
#include <mutex>
#include "esp_http_client.h"
#include "manager_safety.h"

// 天气数据结构
struct ForecastDay {
    std::string date;
    std::string text;
    std::string icon;
    std::string temp_max;
    std::string temp_min;
};

struct WeatherData {
    std::string city;     // 城市名
    std::string temp;     // 温度（字符串）
    std::string text;     // 天气描述（如"晴"、"多云"）
    std::string update_time;
    std::string icon;
    std::string feels_like;
    std::string humidity;
    std::string wind;
    std::array<ForecastDay, 7> forecast;
    int forecast_count = 0;
    bool valid = false;
};

// 天气管理器：通过和风天气坐标接口获取当前天气和七日预报。
class WeatherManager {
public:
    static WeatherManager& getInstance();
    
    // 更新天气数据（包含定位+天气请求，耗时较长，应在后台任务中调用）
    // 返回 true 表示更新成功，false 表示失败（网络错误等）
    bool update();
    
    // 获取最新天气数据（线程安全，直接读取缓存）
    // 即使网络断开，也能返回上次成功获取的数据
    WeatherData getLatestData() const { return latest_data_.Get(); }

    std::string getLocationConfigJson() const;
    std::string GetDiagnosticJson() const;
    bool applyLocationConfigJson(const char* json, std::string& error);
    uint32_t GetRefreshIntervalMinutes() const;
    bool TakeRefreshRequest() { return refresh_requested_.exchange(false); }
    void RequestRefresh() { refresh_requested_ = true; }
    // 刷新状态查询（供 UI 显示"正在刷新…"）
    bool IsRefreshing() const { return is_refreshing_.load(); }
    // 最近一次刷新完成时间（epoch 秒，不管成败）
    int64_t GetLastCompletedAt() const { return last_completed_at_.load(); }

    // 通过外部工具（如 MCP）直接写入天气数据
    // 适用于不走板载 HTTP 天气接口，而由 AI 侧先查好天气再下发到设备
    bool updateFromExternal(const std::string& city,
                            const std::string& weather_text,
                            const std::string& temperature,
                            const std::string& update_time = "");
    static esp_err_t http_event_handler(esp_http_client_event_t *evt);

private:
    struct WeatherConfig {
        std::string province = "江苏省";
        std::string city = "苏州市";
        std::string amap_adcode = "320500";
        std::string amap_key;
        double latitude = 31.2989;
        double longitude = 120.5853;
        uint32_t refresh_interval_minutes = 15;
    };

    WeatherManager();
    rlcd::ThreadSafeSnapshot<WeatherData> latest_data_;
    rlcd::ThreadSafeSnapshot<WeatherConfig> config_;
    mutable std::mutex update_mutex_;
    std::atomic<bool> refresh_requested_{true};
    std::atomic<bool> is_refreshing_{false};
    std::atomic<int64_t> last_completed_at_{0};
    int last_http_status_ = 0;
    std::string last_endpoint_ = "尚未请求";
    std::string last_result_ = "尚未请求";
    bool last_request_ok_ = false;
    
    bool parseAmapNowJson(const char* json_data, const WeatherConfig& config);
    bool parseAmapForecastJson(const char* json_data);
    bool parseOpenMeteoForecastJson(const char* json_data);
    void SetDiagnostic(int status, bool ok, const std::string& result);
};
