#include "calendar_manager.h"

#include <ctime>
#include <cstring>
#include <cmath>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "settings.h"
#include "manager_safety.h"

namespace {
constexpr const char* TAG = "CalendarManager";
std::string response;
uint32_t request_generation = 0;
esp_err_t Event(esp_http_client_event_t* event) {
    if (rlcd::BackgroundNetworkCancelled(request_generation)) return ESP_FAIL;
    if (event->event_id == HTTP_EVENT_ON_DATA && response.size() + event->data_len < 16384) {
        response.append(static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}
std::string ReplaceYear(std::string source, int year) {
    auto pos = source.find("{year}");
    if (pos != std::string::npos) source.replace(pos, 6, std::to_string(year));
    return source;
}
}

CalendarManager& CalendarManager::GetInstance() {
    static CalendarManager instance;
    return instance;
}

void CalendarManager::Init() { Load(); }

int CalendarManager::GetCachedYear() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cached_year_;
}

void CalendarManager::Load() {
    Settings settings("calendar", false);
    source_ = settings.GetString("source", source_);
    cached_year_ = settings.GetInt("year", 0);
    synced_at_ = settings.GetInt("synced", 0);
    std::string raw = settings.GetString("days", "[]");
    cJSON* array = cJSON_Parse(raw.c_str());
    if (!cJSON_IsArray(array)) { if (array) cJSON_Delete(array); return; }
    std::lock_guard<std::mutex> lock(mutex_);
    days_.clear();
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, array) {
        cJSON *date = cJSON_GetObjectItem(item, "date"), *name = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(date) || !cJSON_IsString(name)) continue;
        days_.push_back({date->valuestring, name->valuestring, cJSON_IsTrue(cJSON_GetObjectItem(item, "isOffDay")) != 0});
    }
    cJSON_Delete(array);
}

void CalendarManager::Save() {
    cJSON* array = cJSON_CreateArray();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& day : days_) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "date", day.date.c_str());
            cJSON_AddStringToObject(item, "name", day.name.c_str());
            cJSON_AddBoolToObject(item, "isOffDay", day.off_day);
            cJSON_AddItemToArray(array, item);
        }
    }
    char* raw = cJSON_PrintUnformatted(array);
    Settings settings("calendar", true);
    settings.SetString("source", source_);
    settings.SetInt("year", cached_year_);
    settings.SetInt("synced", synced_at_);
    if (raw) settings.SetString("days", raw);
    if (raw) cJSON_free(raw);
    cJSON_Delete(array);
}

bool CalendarManager::SyncYear(int year) {
    rlcd::BackgroundNetworkSession network_session;
    if (network_session.cancelled()) return false;
    request_generation = network_session.generation();
    response.clear();
    std::string url = ReplaceYear(source_, year);
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = Event;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    auto client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "节假日同步失败: %s HTTP %d", esp_err_to_name(err), status);
        return false;
    }
    cJSON* root = cJSON_Parse(response.c_str());
    cJSON* array = root ? cJSON_GetObjectItem(root, "days") : nullptr;
    if (!cJSON_IsArray(array)) { if (root) cJSON_Delete(root); return false; }
    std::vector<HolidayInfo> next;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, array) {
        cJSON *date = cJSON_GetObjectItem(item, "date"), *name = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(date) && cJSON_IsString(name)) {
            next.push_back({date->valuestring, name->valuestring, cJSON_IsTrue(cJSON_GetObjectItem(item, "isOffDay")) != 0});
        }
    }
    cJSON_Delete(root);
    if (next.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        days_ = std::move(next);
        cached_year_ = year;
        synced_at_ = time(nullptr);
    }
    Save();
    return true;
}

HolidayInfo CalendarManager::Find(const std::string& date) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& day : days_) if (day.date == date) return day;
    return {};
}

std::string CalendarManager::GetConfigJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "source", source_.c_str());
    cJSON_AddNumberToObject(root, "cached_year", cached_year_);
    cJSON_AddNumberToObject(root, "synced_at", synced_at_);
    char* raw = cJSON_PrintUnformatted(root);
    std::string result = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return result;
}

bool CalendarManager::ApplyConfigJson(const char* json, std::string& error) {
    cJSON* root = cJSON_Parse(json);
    cJSON* source = root ? cJSON_GetObjectItem(root, "source") : nullptr;
    if (source && cJSON_IsString(source)) {
        std::string value = source->valuestring;
        if (value.rfind("https://", 0) != 0 || value.find("{year}") == std::string::npos) {
            cJSON_Delete(root); error = "数据源必须为 HTTPS 且包含 {year}"; return false;
        }
        source_ = value;
    }
    cJSON_Delete(root);
    Save();
    return true;
}

namespace {
// Bit-packed Chinese calendar data for 1900-2049. The low nibble is leap month;
// upper bits describe 29/30-day months. Keeping it in flash avoids a daily network dependency.
constexpr uint32_t kLunarInfo[] = {
0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
0x06ca0,0x0b550,0x15355,0x04da0,0x0a5d0,0x14573,0x052d0,0x0a9a8,0x0e950,0x06aa0,
0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b5a0,0x195a6,
0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x055c0,0x0ab60,0x096d5,0x092e0,
0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
0x05aa0,0x076a3,0x096d0,0x04bd7,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0};

int LeapMonth(int y) { return kLunarInfo[y - 1900] & 0xf; }
int LeapDays(int y) { return LeapMonth(y) ? ((kLunarInfo[y - 1900] & 0x10000) ? 30 : 29) : 0; }
int MonthDays(int y, int m) { return (kLunarInfo[y - 1900] & (0x10000 >> m)) ? 30 : 29; }
int YearDays(int y) {
    int sum = 348;
    for (uint32_t mask = 0x8000; mask > 0x8; mask >>= 1) if (kLunarInfo[y - 1900] & mask) ++sum;
    return sum + LeapDays(y);
}
int64_t CivilDays(int y, unsigned m, unsigned d) {
    y -= m <= 2; const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe);
}
bool ConvertLunar(int year, int month, int day, int& lunar_month, int& lunar_day, bool& leap) {
    if (year < 1900 || year >= 2050) return false;
    int64_t offset = CivilDays(year, month, day) - CivilDays(1900, 1, 31);
    int ly = 1900;
    while (ly < 2050 && offset >= YearDays(ly)) offset -= YearDays(ly++);
    int leap_month = LeapMonth(ly); leap = false;
    for (lunar_month = 1; lunar_month <= 12; ++lunar_month) {
        int days = leap ? LeapDays(ly) : MonthDays(ly, lunar_month);
        if (offset < days) break;
        offset -= days;
        if (leap) leap = false;
        else if (leap_month == lunar_month) { leap = true; --lunar_month; }
    }
    lunar_day = static_cast<int>(offset) + 1;
    return lunar_month <= 12;
}

const char* LunarFestival(int month, int day, bool leap) {
    if (leap) return nullptr;
    struct Festival { int month; int day; const char* name; };
    static constexpr Festival festivals[] = {
        {1, 1, "春节"}, {1, 15, "元宵节"}, {2, 2, "龙抬头"},
        {5, 5, "端午节"}, {7, 7, "七夕节"}, {7, 15, "中元节"},
        {8, 15, "中秋节"}, {9, 9, "重阳节"}, {12, 8, "腊八节"},
    };
    for (const auto& festival : festivals) {
        if (festival.month == month && festival.day == day) return festival.name;
    }
    return nullptr;
}

const char* SolarTerm(int year, int month, int day) {
    // Minutes after 1900-01-06 02:05 China Standard Time for each solar longitude term.
    // The tropical-year approximation is valid for this firmware's 1900-2049 range.
    static constexpr int minutes[] = {
        0, 21208, 42467, 63836, 85337, 107014, 128867, 150921,
        173149, 195551, 218072, 240693, 263343, 285989, 308563, 331033,
        353350, 375494, 397447, 419210, 440795, 462224, 483532, 504758,
    };
    static constexpr const char* names[] = {
        "小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明", "谷雨",
        "立夏", "小满", "芒种", "夏至", "小暑", "大暑", "立秋", "处暑",
        "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至",
    };
    // The conventional base is 1900-01-06 02:05 China Standard Time.
    static constexpr int64_t base_epoch = -2208578100LL;
    for (int index = 0; index < 24; ++index) {
        const int term_month = index / 2 + 1;
        if (term_month != month) continue;
        const double elapsed_ms = 31556925974.7 * (year - 1900) + minutes[index] * 60000.0;
        time_t china_epoch = static_cast<time_t>(base_epoch + std::llround(elapsed_ms / 1000.0) + 8 * 3600);
        struct tm china = {};
        gmtime_r(&china_epoch, &china);
        if (china.tm_mday == day) return names[index];
    }
    return nullptr;
}
const char* DayText(int day) {
    static const char* cn_day[] = {"初一","初二","初三","初四","初五","初六","初七","初八","初九","初十",
        "十一","十二","十三","十四","十五","十六","十七","十八","十九","二十","廿一","廿二","廿三","廿四",
        "廿五","廿六","廿七","廿八","廿九","三十"};
    return day >= 1 && day <= 30 ? cn_day[day - 1] : "--";
}
}

std::string CalendarManager::LunarText(int year, int month, int day) {
    int lm = 0, ld = 0; bool leap = false;
    return ConvertLunar(year, month, day, lm, ld, leap) ? DayText(ld) : "--";
}

std::string CalendarManager::LunarFullText(int year, int month, int day) {
    static const char* months[] = {"正月","二月","三月","四月","五月","六月","七月","八月","九月","十月","冬月","腊月"};
    int lm = 0, ld = 0; bool leap = false;
    if (!ConvertLunar(year, month, day, lm, ld, leap)) return "--";
    return std::string(leap ? "闰" : "") + months[lm - 1] + DayText(ld);
}

std::string CalendarManager::TraditionalDayText(int year, int month, int day) {
    int lunar_month = 0;
    int lunar_day = 0;
    bool leap = false;
    if (ConvertLunar(year, month, day, lunar_month, lunar_day, leap)) {
        if (const char* festival = LunarFestival(lunar_month, lunar_day, leap)) return festival;
    }
    if (const char* term = SolarTerm(year, month, day)) return term;
    return {};
}
