// QWEATHER_CITY_WEATHER_V1

#include "weather_manager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <miniz.h>

#include "settings.h"
#include "manager_safety.h"

namespace {
constexpr const char* TAG = "WeatherManager";
// 和风七日预报响应字段较丰富，缓冲从 8KB 提到 12KB（PSRAM，仍低于 16KB 上限）
constexpr int kResponseBufferSize = 12288;
// 和风网关强制 gzip 压缩（实测无视 Accept-Encoding: identity），需板载解压
constexpr int kInflatedBufferSize = 16384;
char* response_buffer = nullptr;
char* inflated_buffer = nullptr;
// tinfl_decompressor 状态结构约 3.3KB，一次性放 PSRAM——绝不能上任务栈
//（activation 任务栈余量小，mem_to_mem 帮助函数把它放栈上已实测溢出）
tinfl_decompressor* decompressor_state = nullptr;
const char* response_body = nullptr;  // RequestJson 成功后指向可解析文本（gzip 时指向解压缓冲）
int response_len = 0;
uint32_t request_generation = 0;

std::string JsonString(cJSON* object, const char* key) {
    cJSON* item = object ? cJSON_GetObjectItem(object, key) : nullptr;
    return cJSON_IsString(item) ? item->valuestring : "";
}

const char* RequestFailureMessage(int status) {
    if (status == 401 || status == 403) return "认证失败：请检查和风 API Host 与 Key";
    if (status == 429) return "请求过于频繁或已超限，请稍后重试";
    return "请求失败：请检查网络、和风 API Host 与 Key 配置";
}

// 和风 wind.direction.compass 是英文方位码（n/nne/.../vrb），转成中文方位
const char* CompassToChinese(const std::string& compass) {
    static const std::pair<const char*, const char*> kDirections[] = {
        {"n", "北"},   {"nne", "北东北"}, {"ne", "东北"}, {"ene", "东东北"},
        {"e", "东"},   {"ese", "东东南"}, {"se", "东南"}, {"sse", "南东南"},
        {"s", "南"},   {"ssw", "南西南"}, {"sw", "西南"}, {"wsw", "西西南"},
        {"w", "西"},   {"wnw", "西西北"}, {"nw", "西北"}, {"nnw", "北西北"},
        {"none", "静"}, {"vrb", "不定"},
    };
    for (const auto& entry : kDirections) {
        if (compass == entry.first) return entry.second;
    }
    return "";
}

std::string RoundedNumber(cJSON* item) {
    if (!cJSON_IsNumber(item)) return {};
    return std::to_string(static_cast<int>(std::lround(item->valuedouble)));
}

// 取 {value, unit} 数值对象的四舍五入整数字符串（和风数值字段的统一格式）
std::string RoundedValue(cJSON* object, const char* key) {
    cJSON* child = object ? cJSON_GetObjectItem(object, key) : nullptr;
    return RoundedNumber(cJSON_IsObject(child) ? cJSON_GetObjectItem(child, "value") : nullptr);
}

// 手工跳过 gzip 头（ROM tinfl 只认裸 deflate 流），再一次解压到 PSRAM 缓冲
bool GunzipResponse() {
    if (response_len < 18 || (uint8_t)response_buffer[0] != 0x1f ||
        (uint8_t)response_buffer[1] != 0x8b || (uint8_t)response_buffer[2] != 8) {
        return false;
    }
    const uint8_t flags = (uint8_t)response_buffer[3];
    int pos = 10;
    if (flags & 0x04) {  // FEXTRA：2 字节长度 + 数据
        if (pos + 2 > response_len) return false;
        pos += 2 + ((uint8_t)response_buffer[pos] | ((uint8_t)response_buffer[pos + 1] << 8));
    }
    if (flags & 0x08) {  // FNAME：NUL 结尾字符串
        while (pos < response_len && response_buffer[pos]) ++pos;
        ++pos;
    }
    if (flags & 0x10) {  // FCOMMENT：同上
        while (pos < response_len && response_buffer[pos]) ++pos;
        ++pos;
    }
    if (flags & 0x02) pos += 2;  // FHCRC
    if (!inflated_buffer || !decompressor_state || pos >= response_len) return false;
    tinfl_init(decompressor_state);
    size_t in_len = response_len - pos;
    size_t out_len = kInflatedBufferSize - 1;
    const tinfl_status result = tinfl_decompress(
        decompressor_state, (const mz_uint8*)(response_buffer + pos), &in_len,
        (mz_uint8*)inflated_buffer, (mz_uint8*)inflated_buffer, &out_len,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (result != TINFL_STATUS_DONE || out_len == 0) return false;
    inflated_buffer[out_len] = '\0';
    response_body = inflated_buffer;
    return true;
}

bool RequestJson(const char* url, const std::string& api_key, int& status) {
    response_len = 0;
    memset(response_buffer, 0, kResponseBufferSize);
    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = WeatherManager::http_event_handler;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    auto client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "xiaozhi-rlcd-weather/3.0");
    // API KEY 走请求头，不进 URL，避免出现在诊断与日志里
    if (!api_key.empty()) {
        esp_http_client_set_header(client, "X-QW-Api-Key", api_key.c_str());
    }
    const esp_err_t error = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK || status != 200 || response_len <= 0) {
        ESP_LOGW(TAG, "天气请求失败: err=%s status=%d", esp_err_to_name(error), status);
        return false;
    }
    response_buffer[response_len] = '\0';
    response_body = response_buffer;
    if ((uint8_t)response_buffer[0] == 0x1f && (uint8_t)response_buffer[1] == 0x8b &&
        !GunzipResponse()) {
        ESP_LOGW(TAG, "天气响应 gzip 解压失败");
        return false;
    }
    return true;
}
}  // namespace

esp_err_t WeatherManager::http_event_handler(esp_http_client_event_t* event) {
    if (rlcd::BackgroundNetworkCancelled(request_generation)) return ESP_FAIL;
    if (event->event_id == HTTP_EVENT_ON_DATA && response_buffer &&
        response_len + event->data_len < kResponseBufferSize - 1) {
        memcpy(response_buffer + response_len, event->data, event->data_len);
        response_len += event->data_len;
    }
    return ESP_OK;
}

WeatherManager::WeatherManager() {
    // PSRAM 优先，失败时报错而不是 fallback 到内部 SRAM——内部 SRAM 紧张（3%），
    // 数十 KB 缓冲区会耗尽它导致后台/Wi-Fi 崩溃。宁可天气失败也不能耗 SRAM。
    response_buffer = static_cast<char*>(heap_caps_malloc(kResponseBufferSize, MALLOC_CAP_SPIRAM));
    if (!response_buffer) {
        ESP_LOGE(TAG, "天气响应缓冲区 PSRAM 分配失败（12KB），天气功能将不可用");
    }
    inflated_buffer = static_cast<char*>(heap_caps_malloc(kInflatedBufferSize, MALLOC_CAP_SPIRAM));
    if (!inflated_buffer) {
        ESP_LOGE(TAG, "天气解压缓冲区 PSRAM 分配失败（16KB），gzip 响应将无法解析");
    }
    decompressor_state = static_cast<tinfl_decompressor*>(
        heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM));
    if (!decompressor_state) {
        ESP_LOGE(TAG, "天气解压状态 PSRAM 分配失败（%u 字节），gzip 响应将无法解析",
                 (unsigned)sizeof(tinfl_decompressor));
    }
    Settings settings("weather", false);
    WeatherConfig config;
    config.province = settings.GetString("province", "江苏省");
    config.city = settings.GetString("city", "苏州市");
    config.qw_host = settings.GetString("qw_host", "");
    config.qw_key = settings.GetString("qw_key", "");
    config.latitude = strtod(settings.GetString("latitude", "31.2989").c_str(), nullptr);
    config.longitude = strtod(settings.GetString("longitude", "120.5853").c_str(), nullptr);
    const int saved_refresh_minutes = settings.GetInt("refresh_minutes", 15);
    if (saved_refresh_minutes >= 5 && saved_refresh_minutes <= 120) {
        config.refresh_interval_minutes = saved_refresh_minutes;
    }
    config_.Set(std::move(config));
    // 一次性清理 2026-08-17 切换和风前遗留的高德配置（含 secret，不再使用）
    const bool has_legacy = !settings.GetString("amap_key", "").empty() ||
                            !settings.GetString("amap_adcode", "").empty();
    if (has_legacy) {
        Settings writable("weather", true);
        writable.EraseKey("amap_key");
        writable.EraseKey("amap_adcode");
        ESP_LOGI(TAG, "已清理遗留的高德天气配置");
    }
}

WeatherManager& WeatherManager::getInstance() {
    static WeatherManager instance;
    return instance;
}

std::string WeatherManager::getLocationConfigJson() const {
    const WeatherConfig config = config_.Get();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "province", config.province.c_str());
    cJSON_AddStringToObject(root, "city", config.city.c_str());
    cJSON_AddStringToObject(root, "name", config.city.c_str());
    cJSON_AddStringToObject(root, "provider", "qweather");
    cJSON_AddStringToObject(root, "qw_host", config.qw_host.c_str());
    cJSON_AddNumberToObject(root, "latitude", config.latitude);
    cJSON_AddNumberToObject(root, "longitude", config.longitude);
    cJSON_AddBoolToObject(root, "has_qw_key", !config.qw_key.empty());
    cJSON_AddNumberToObject(root, "refresh_interval_minutes", config.refresh_interval_minutes);
    cJSON_AddBoolToObject(root, "refreshing", is_refreshing_.load());
    cJSON_AddNumberToObject(root, "last_refresh_completed_at",
                            static_cast<double>(last_completed_at_.load()));
    char* raw = cJSON_PrintUnformatted(root);
    std::string output = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return output;
}

std::string WeatherManager::GetDiagnosticJson() const {
    std::lock_guard<std::mutex> lock(update_mutex_);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", last_request_ok_);
    cJSON_AddNumberToObject(root, "http_status", last_http_status_);
    cJSON_AddStringToObject(root, "endpoint", last_endpoint_.c_str());
    cJSON_AddStringToObject(root, "result", last_result_.c_str());
    char* raw = cJSON_PrintUnformatted(root);
    std::string output = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return output;
}

uint32_t WeatherManager::GetRefreshIntervalMinutes() const {
    return config_.Get().refresh_interval_minutes;
}

bool WeatherManager::applyLocationConfigJson(const char* json, std::string& error) {
    cJSON* root = cJSON_Parse(json);
    cJSON* province = root ? cJSON_GetObjectItem(root, "province") : nullptr;
    cJSON* city = root ? cJSON_GetObjectItem(root, "city") : nullptr;
    cJSON* qw_host = root ? cJSON_GetObjectItem(root, "qw_host") : nullptr;
    cJSON* qw_key = root ? cJSON_GetObjectItem(root, "qw_key") : nullptr;
    cJSON* latitude = root ? cJSON_GetObjectItem(root, "latitude") : nullptr;
    cJSON* longitude = root ? cJSON_GetObjectItem(root, "longitude") : nullptr;
    cJSON* refresh_minutes = root ? cJSON_GetObjectItem(root, "refresh_interval_minutes") : nullptr;
    cJSON* clear_qw_key = root ? cJSON_GetObjectItem(root, "clear_qw_key") : nullptr;
    // Host 允许为空（保存后 update 会提示未配置），非空时必须是合法主机名字符
    const bool host_valid =
        !cJSON_IsString(qw_host) ||
        (strlen(qw_host->valuestring) <= 100 &&
         std::all_of(qw_host->valuestring, qw_host->valuestring + strlen(qw_host->valuestring),
                     [](char c) { return ::isalnum(c) || c == '-' || c == '.'; }));
    if (!cJSON_IsString(province) || !cJSON_IsString(city) ||
        !cJSON_IsNumber(latitude) || !cJSON_IsNumber(longitude) ||
        strlen(province->valuestring) == 0 ||
        strlen(province->valuestring) > 32 || strlen(city->valuestring) == 0 ||
        strlen(city->valuestring) > 32 || !host_valid ||
        latitude->valuedouble < -90 || latitude->valuedouble > 90 ||
        longitude->valuedouble < -180 || longitude->valuedouble > 180) {
        if (root) cJSON_Delete(root);
        error = "请选择有效的国内城市并检查和风 API Host 格式";
        return false;
    }
    WeatherConfig config = config_.Get();
    config.province = province->valuestring;
    config.city = city->valuestring;
    if (cJSON_IsString(qw_host)) config.qw_host = qw_host->valuestring;
    config.latitude = latitude->valuedouble;
    config.longitude = longitude->valuedouble;
    if (cJSON_IsTrue(clear_qw_key)) {
        config.qw_key.clear();
    } else if (cJSON_IsString(qw_key) && strlen(qw_key->valuestring) > 0 &&
               strlen(qw_key->valuestring) <= 128) {
        config.qw_key = qw_key->valuestring;
    }
    if (!cJSON_IsNumber(refresh_minutes) || refresh_minutes->valuedouble < 5 ||
        refresh_minutes->valuedouble > 120 ||
        refresh_minutes->valuedouble != static_cast<int>(refresh_minutes->valuedouble)) {
        cJSON_Delete(root); error = "天气刷新间隔必须为 5-120 分钟"; return false;
    }
    config.refresh_interval_minutes = static_cast<uint32_t>(refresh_minutes->valuedouble);
    cJSON_Delete(root);
    Settings settings("weather", true);
    settings.SetString("province", config.province);
    settings.SetString("city", config.city);
    settings.SetString("qw_host", config.qw_host);
    settings.SetString("qw_key", config.qw_key);
    char coordinate[24];
    snprintf(coordinate, sizeof(coordinate), "%.6f", config.latitude);
    settings.SetString("latitude", coordinate);
    snprintf(coordinate, sizeof(coordinate), "%.6f", config.longitude);
    settings.SetString("longitude", coordinate);
    settings.SetInt("refresh_minutes", config.refresh_interval_minutes);
    config_.Set(std::move(config));
    refresh_requested_ = true;
    return true;
}

bool WeatherManager::updateFromExternal(const std::string& city, const std::string& weather_text,
                                        const std::string& temperature, const std::string& update_time) {
    if (city.empty() || weather_text.empty() || temperature.empty()) return false;
    latest_data_.Modify([&](WeatherData& data) {
        data.city = city; data.text = weather_text; data.temp = temperature;
        data.update_time = update_time.empty() ? "mcp" : update_time; data.valid = true;
    });
    return true;
}

bool WeatherManager::update() {
    rlcd::BackgroundNetworkSession network_session;
    if (network_session.cancelled()) return false;
    request_generation = network_session.generation();
    std::lock_guard<std::mutex> update_lock(update_mutex_);
    is_refreshing_.store(true);
    // RAII：函数返回时（无论哪条路径）清除 refreshing 并记录完成时间
    struct RefreshGuard {
        WeatherManager* self;
        ~RefreshGuard() {
            self->is_refreshing_.store(false);
            self->last_completed_at_.store(time(nullptr));
        }
    } guard{this};
    const WeatherConfig config = config_.Get();
    if (!response_buffer || config.qw_host.empty() || config.qw_key.empty()) {
        const char* message = config.qw_host.empty() ? "未配置和风 API Host" : "未配置和风 API Key";
        if (!response_buffer) message = "天气响应缓冲区不可用";
        ESP_LOGW(TAG, "%s", message);
        SetDiagnostic(0, false, message);
        return false;
    }
    char url[768];
    int status = 0;
    // 和风路径参数要求经纬度最多两位小数
    last_endpoint_ = "实时天气 /weather/v1/current";
    snprintf(url, sizeof(url), "https://%s/weather/v1/current/%.2f/%.2f",
             config.qw_host.c_str(), config.latitude, config.longitude);
    if (!RequestJson(url, config.qw_key, status)) {
        SetDiagnostic(status, false, RequestFailureMessage(status));
        return false;
    }
    if (network_session.cancelled()) return false;
    if (!parseQweatherCurrentJson(response_body, config)) {
        SetDiagnostic(status, false, "和风实时天气响应无效");
        return false;
    }
    last_endpoint_ = "七日天气 /weather/v1/daily?days=7";
    snprintf(url, sizeof(url), "https://%s/weather/v1/daily/%.2f/%.2f?days=7&localTime=true",
             config.qw_host.c_str(), config.latitude, config.longitude);
    if (RequestJson(url, config.qw_key, status) && !network_session.cancelled() &&
        parseQweatherDailyJson(response_body)) {
        SetDiagnostic(status, true, "和风实时天气与七日预报更新成功");
        return true;
    }
    if (network_session.cancelled()) return false;

    // 单源无降级：实时已更新，七日保留上次成功快照，报失败让调度器稍后重试
    SetDiagnostic(status, false, "和风实时天气成功；七日预报失败，保留上次预报并将在空闲时重试");
    return false;
}

void WeatherManager::SetDiagnostic(int status, bool ok, const std::string& result) {
    last_http_status_ = status;
    last_request_ok_ = ok;
    last_result_ = result;
}

bool WeatherManager::parseQweatherCurrentJson(const char* json_data, const WeatherConfig& config) {
    // 新版实时接口为顶层平铺结构：condition{text,code}、temperature{value,unit} 等
    cJSON* root = cJSON_Parse(json_data);
    cJSON* condition = root ? cJSON_GetObjectItem(root, "condition") : nullptr;
    const std::string text = JsonString(condition, "text");
    const std::string temp = RoundedValue(root, "temperature");
    if (text.empty() || temp.empty()) {
        // 留证：gzip 魔数 1f 8b 或错误 JSON 都能从头部看出来
        ESP_LOGW(TAG, "和风实时天气响应无效, head=%02x%02x body: %.200s",
                 (uint8_t)json_data[0], (uint8_t)json_data[1], json_data);
        if (root) cJSON_Delete(root);
        return false;
    }
    WeatherData data = latest_data_.Get();
    data.city = config.city;
    data.temp = temp;
    data.text = text;
    data.icon = JsonString(condition, "code");
    data.feels_like = RoundedValue(root, "feelsLike");
    if (data.feels_like.empty()) data.feels_like = data.temp;
    // 湿度是 [0,1] 小数，显示为百分比整数
    cJSON* humidity = cJSON_GetObjectItem(root, "humidity");
    data.humidity = cJSON_IsNumber(humidity)
                        ? std::to_string(static_cast<int>(std::lround(humidity->valuedouble * 100)))
                        : "";
    cJSON* wind = cJSON_GetObjectItem(root, "wind");
    cJSON* direction = cJSON_IsObject(wind) ? cJSON_GetObjectItem(wind, "direction") : nullptr;
    const char* compass = CompassToChinese(JsonString(direction, "compass"));
    cJSON* scale = cJSON_IsObject(wind) ? cJSON_GetObjectItem(wind, "scale") : nullptr;
    data.wind = std::string(compass) + "风";
    if (cJSON_IsNumber(scale)) data.wind += std::to_string(scale->valueint) + "级";
    // 新版实时接口不返回观测时间，用本地请求时刻作为"上次更新"
    char updated[24];
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    strftime(updated, sizeof(updated), "%Y-%m-%d %H:%M", &local);
    data.update_time = updated;
    data.valid = true;
    cJSON_Delete(root);
    latest_data_.Set(std::move(data));
    return true;
}

bool WeatherManager::parseQweatherDailyJson(const char* json_data) {
    cJSON* root = cJSON_Parse(json_data);
    cJSON* days = root ? cJSON_GetObjectItem(root, "days") : nullptr;
    constexpr int kForecastDays = 7;
    if (!cJSON_IsArray(days) || cJSON_GetArraySize(days) < kForecastDays) {
        ESP_LOGW(TAG, "和风七日预报响应无效, head=%02x%02x body: %.200s",
                 (uint8_t)json_data[0], (uint8_t)json_data[1], json_data);
        if (root) cJSON_Delete(root);
        return false;
    }

    std::array<ForecastDay, kForecastDays> parsed;
    for (int index = 0; index < kForecastDays; ++index) {
        cJSON* day = cJSON_GetArrayItem(days, index);
        cJSON* daytime = cJSON_IsObject(day) ? cJSON_GetObjectItem(day, "daytime") : nullptr;
        cJSON* condition =
            cJSON_IsObject(daytime) ? cJSON_GetObjectItem(daytime, "condition") : nullptr;
        // localTime=true 时 forecastStartTime 为本地 ISO 时间，前 10 字符即日期
        const std::string start = JsonString(day, "forecastStartTime");
        const std::string text = JsonString(condition, "text");
        const std::string maximum = RoundedValue(day, "temperatureMax");
        const std::string minimum = RoundedValue(day, "temperatureMin");
        if (start.size() < 10 || text.empty() || maximum.empty() || minimum.empty()) {
            cJSON_Delete(root);
            return false;
        }
        parsed[index] = {start.substr(0, 10), text, JsonString(condition, "code"), maximum,
                         minimum};
    }
    cJSON_Delete(root);
    latest_data_.Modify([&](WeatherData& data) {
        data.forecast = std::move(parsed);
        data.forecast_count = 7;
    });
    ESP_LOGI(TAG, "和风七日天气更新成功");
    return true;
}
