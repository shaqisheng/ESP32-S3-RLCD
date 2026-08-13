#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct QuotaTier {
    std::string label;
    int used_percent = -1;
    double total = 0;
    double remaining = 0;
    std::string unit;
    int64_t reset_at = 0;
};

struct QuotaCard {
    uint32_t id = 0;
    std::string name;
    std::string provider;
    bool enabled = true;
    bool valid = false;
    bool stale = false;
    std::string error;
    int64_t checked_at = 0;
    int64_t success_at = 0;
    std::vector<QuotaTier> tiers;
};

struct QuotaPageSetting {
    std::string id;
    bool enabled = true;
    int order = 0;
};

class QuotaManager {
public:
    static QuotaManager& GetInstance();

    bool Init();
    void Start();
    void RequestRefresh();
    // 请求只刷新指定 entry id 的账号（其他账号跳过）
    void RequestRefreshOne(uint32_t id) { refresh_target_id_.store(id); refresh_requested_.store(true); }

    std::vector<QuotaCard> GetCards() const;
    std::vector<QuotaPageSetting> GetPageSettings() const;
    uint32_t GetRevision() const { return revision_.load(); }
    int64_t GetLastAllSuccessAt() const;
    // 最近一次刷新完成时间（不管成败）。比 last_all_success_at_ 更适合 UI 显示
    // "上次更新时间"——不会因为某一个账号失败就停更时间戳。
    int64_t GetLastRefreshCompletedAt() const { return last_refresh_completed_at_; }
    bool IsRefreshing() const { return refreshing_.load(); }
    uint32_t GetRefreshIntervalMinutes() const;
    bool SetRefreshIntervalMinutes(uint32_t minutes, std::string& error);

    // AI 页显示配置
    uint8_t GetCardsPerPage() const { return cards_per_page_; }
    uint8_t GetAutoAdvanceSeconds() const { return auto_advance_seconds_; }
    uint8_t GetForcePage() const { return force_page_; }
    bool SetDisplayConfig(uint8_t cards_per_page, uint8_t auto_advance_seconds,
                          uint8_t force_page, std::string& error);

    // Returned JSON never includes stored credentials.
    std::string GetConfigJson() const;
    std::string GetProxyDiagnosticJson() const;
    bool ApplyConfigJson(const char* json, std::string& error);
    std::string GetPageConfigJson() const;
    bool ApplyPageConfigJson(const char* json, std::string& error);
    std::string GetStatusJson() const;

private:
    struct Entry {
        uint32_t id = 0;
        bool enabled = true;
        int order = 0;
        std::string name;
        std::string provider;
        std::string base_url;
        bool proxy_enabled = false;
        std::string proxy_url;
        std::string secret;
        std::string account_id;
        std::string unit;
        std::string label;
        std::string total_field;
        std::string remaining_field;
        double manual_total = 0;
        double manual_remaining = 0;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::vector<QuotaCard> cards_;
    std::vector<QuotaPageSetting> pages_;
    std::atomic<uint32_t> revision_{1};
    std::atomic<bool> refresh_requested_{false};
    std::atomic<uint32_t> refresh_target_id_{0};  // 0=全部，非 0=只刷指定 entry id
    std::atomic<bool> refreshing_{false};
    TaskHandle_t task_ = nullptr;
    int64_t last_all_success_at_ = 0;
    int64_t last_refresh_completed_at_ = 0;  // 最近一次刷新完成（不管成败）
    uint32_t refresh_interval_minutes_ = 5;
    // AI 页显示配置（NVS quota namespace 持久化）
    uint8_t cards_per_page_ = 4;          // 1-4，每屏几张卡片
    uint8_t auto_advance_seconds_ = 10;   // 0=不翻页，否则 N 秒后自动切下一页
    uint8_t force_page_ = 0;              // 0=自动翻页，1-N=固定显示第 N 页
    bool initialized_ = false;

    QuotaManager() = default;
    static void TaskMain(void* arg);
    void Run();
    void RefreshAll();
    bool RefreshOne(const Entry& entry, QuotaCard& card, bool& transient);
    bool HttpGet(const Entry& entry, const std::string& url, std::string& body,
                 int& status, std::string& error);
    bool ParseResponse(const Entry& entry, const char* json, QuotaCard& card,
                       std::string& error);
    void Load();
    bool SaveEntriesLocked(std::string& error);
    bool SavePagesLocked(std::string& error);
    void LoadCacheLocked();
    void SaveCardCache(const QuotaCard& card);
};
