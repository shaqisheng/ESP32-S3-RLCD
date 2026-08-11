#pragma once

#include <mutex>
#include <string>
#include <vector>

struct HolidayInfo {
    std::string date;
    std::string name;
    bool off_day = false;
};

class CalendarManager {
public:
    static CalendarManager& GetInstance();
    void Init();
    bool SyncYear(int year);
    int GetCachedYear() const;
    HolidayInfo Find(const std::string& date) const;
    std::string GetConfigJson() const;
    bool ApplyConfigJson(const char* json, std::string& error);
    static std::string LunarText(int year, int month, int day);
    static std::string LunarFullText(int year, int month, int day);
    static std::string TraditionalDayText(int year, int month, int day);

private:
    CalendarManager() = default;
    mutable std::mutex mutex_;
    std::vector<HolidayInfo> days_;
    std::string source_ = "https://raw.githubusercontent.com/NateScarlet/holiday-cn/master/{year}.json";
    int cached_year_ = 0;
    long synced_at_ = 0;
    void Load();
    void Save();
};
