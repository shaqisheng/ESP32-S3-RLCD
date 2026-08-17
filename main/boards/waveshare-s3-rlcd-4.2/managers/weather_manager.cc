// AMAP_CITY_WEATHER_V1

#include "weather_manager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include "settings.h"
#include "manager_safety.h"

namespace {
constexpr const char* TAG = "WeatherManager";
constexpr int kResponseBufferSize = 8192;
char* response_buffer = nullptr;
int response_len = 0;
uint32_t request_generation = 0;

std::string JsonString(cJSON* object, const char* key) {
    cJSON* item = object ? cJSON_GetObjectItem(object, key) : nullptr;
    return cJSON_IsString(item) ? item->valuestring : "";
}

const char* RequestFailureMessage(int status) {
    if (status == 401 || status == 403) return "认证失败：请检查高德 Web 服务 Key";
    if (status == 429) return "请求过于频繁或已超限，请稍后重试";
    return "请求失败：请检查网络、高德 Key 与配置";
}

const char* WmoWeatherText(int code) {
    if (code == 0) return "晴";
    if (code <= 3) return "多云";
    if (code == 45 || code == 48) return "雾";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "雨";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "雪";
    if (code >= 95) return "雷雨";
    return "多云";
}

std::string RoundedNumber(cJSON* item) {
    if (!cJSON_IsNumber(item)) return {};
    return std::to_string(static_cast<int>(std::lround(item->valuedouble)));
}

bool RequestJson(const char* url, int& status) {
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
    esp_http_client_set_header(client, "User-Agent", "xiaozhi-rlcd-weather/2.0");
    const esp_err_t error = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK || status != 200 || response_len <= 0) {
        ESP_LOGW(TAG, "天气请求失败: err=%s status=%d", esp_err_to_name(error), status);
        return false;
    }
    response_buffer[response_len] = '\0';
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
    // 16KB 缓冲区会耗尽它导致后台/Wi-Fi 崩溃。宁可天气失败也不能耗 SRAM。
    response_buffer = static_cast<char*>(heap_caps_malloc(kResponseBufferSize, MALLOC_CAP_SPIRAM));
    if (!response_buffer) {
        ESP_LOGE(TAG, "天气响应缓冲区 PSRAM 分配失败（16KB），天气功能将不可用");
    }
    Settings settings("weather", false);
    WeatherConfig config;
    config.province = settings.GetString("province", "江苏省");
    config.city = settings.GetString("city", "苏州市");
    config.amap_adcode = settings.GetString("amap_adcode", "320500");
    config.amap_key = settings.GetString("amap_key", "");
    config.latitude = strtod(settings.GetString("latitude", "31.2989").c_str(), nullptr);
    config.longitude = strtod(settings.GetString("longitude", "120.5853").c_str(), nullptr);
    const int saved_refresh_minutes = settings.GetInt("refresh_minutes", 15);
    if (saved_refresh_minutes >= 5 && saved_refresh_minutes <= 120) {
        config.refresh_interval_minutes = saved_refresh_minutes;
    }
    config_.Set(std::move(config));
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
    cJSON_AddStringToObject(root, "provider", "amap");
    cJSON_AddStringToObject(root, "amap_adcode", config.amap_adcode.c_str());
    cJSON_AddNumberToObject(root, "latitude", config.latitude);
    cJSON_AddNumberToObject(root, "longitude", config.longitude);
    cJSON_AddBoolToObject(root, "has_amap_key", !config.amap_key.empty());
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
    cJSON* amap_adcode = root ? cJSON_GetObjectItem(root, "amap_adcode") : nullptr;
    cJSON* amap_key = root ? cJSON_GetObjectItem(root, "amap_key") : nullptr;
    cJSON* latitude = root ? cJSON_GetObjectItem(root, "latitude") : nullptr;
    cJSON* longitude = root ? cJSON_GetObjectItem(root, "longitude") : nullptr;
    cJSON* refresh_minutes = root ? cJSON_GetObjectItem(root, "refresh_interval_minutes") : nullptr;
    cJSON* clear_amap_key = root ? cJSON_GetObjectItem(root, "clear_amap_key") : nullptr;
    if (!cJSON_IsString(province) || !cJSON_IsString(city) || !cJSON_IsString(amap_adcode) ||
        !cJSON_IsNumber(latitude) || !cJSON_IsNumber(longitude) ||
        strlen(province->valuestring) == 0 ||
        strlen(province->valuestring) > 32 || strlen(city->valuestring) == 0 ||
        strlen(city->valuestring) > 32 || strlen(amap_adcode->valuestring) != 6 ||
        !std::all_of(amap_adcode->valuestring, amap_adcode->valuestring + 6, ::isdigit) ||
        latitude->valuedouble < -90 || latitude->valuedouble > 90 ||
        longitude->valuedouble < -180 || longitude->valuedouble > 180) {
        if (root) cJSON_Delete(root);
        error = "请选择有效的国内城市";
        return false;
    }
    WeatherConfig config = config_.Get();
    config.province = province->valuestring;
    config.city = city->valuestring;
    config.amap_adcode = amap_adcode->valuestring;
    config.latitude = latitude->valuedouble;
    config.longitude = longitude->valuedouble;
    if (cJSON_IsTrue(clear_amap_key)) config.amap_key.clear();
    else if (cJSON_IsString(amap_key) && strlen(amap_key->valuestring) > 0 && strlen(amap_key->valuestring) <= 128) config.amap_key = amap_key->valuestring;
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
    settings.SetString("amap_adcode", config.amap_adcode);
    settings.SetString("amap_key", config.amap_key);
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
    if (!response_buffer || config.amap_key.empty() || config.amap_adcode.size() != 6) {
        const char* message = config.amap_key.empty() ? "未配置高德 Web 服务 Key" : "未配置高德城市 adcode";
        ESP_LOGW(TAG, "%s", message);
        SetDiagnostic(0, false, message);
        return false;
    }
    char url[768];
    int status = 0;
    last_endpoint_ = "实时天气 /v3/weather/weatherInfo?extensions=base";
    snprintf(url, sizeof(url), "https://restapi.amap.com/v3/weather/weatherInfo?city=%s&key=%s&extensions=base",
             config.amap_adcode.c_str(), config.amap_key.c_str());
    if (!RequestJson(url, status)) {
        SetDiagnostic(status, false, RequestFailureMessage(status));
        return false;
    }
    if (network_session.cancelled()) return false;
    if (!parseAmapNowJson(response_buffer, config)) { SetDiagnostic(status, false, "高德实时天气响应无效"); return false; }
    last_endpoint_ = "七日天气 api.open-meteo.com/v1/forecast";
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=Asia%%2FShanghai&forecast_days=7",
             config.latitude, config.longitude);
    if (RequestJson(url, status) && !network_session.cancelled() &&
        parseOpenMeteoForecastJson(response_buffer)) {
        SetDiagnostic(status, true, "高德实时天气与 Open-Meteo 七日预报更新成功");
        return true;
    }
    if (network_session.cancelled()) return false;

    // Amap officially provides only today, day two and day three. Keep those
    // visible as a fallback, but report failure so the scheduler retries the
    // seven-day source after its shorter retry interval.
    const int seven_day_status = status;
    last_endpoint_ = "七日天气失败，已降级高德三日预报";
    snprintf(url, sizeof(url), "https://restapi.amap.com/v3/weather/weatherInfo?city=%s&key=%s&extensions=all",
             config.amap_adcode.c_str(), config.amap_key.c_str());
    if (RequestJson(url, status) && !network_session.cancelled()) {
        parseAmapForecastJson(response_buffer);
    }
    SetDiagnostic(seven_day_status, false, "高德实时天气成功；七日预报失败，已降级显示三日并将在空闲时重试");
    return false;
}

void WeatherManager::SetDiagnostic(int status, bool ok, const std::string& result) {
    last_http_status_ = status;
    last_request_ok_ = ok;
    last_result_ = result;
}

bool WeatherManager::parseAmapNowJson(const char* json_data, const WeatherConfig& config) {
    cJSON* root = cJSON_Parse(json_data);
    cJSON* lives = root ? cJSON_GetObjectItem(root, "lives") : nullptr;
    cJSON* live = cJSON_IsArray(lives) ? cJSON_GetArrayItem(lives, 0) : nullptr;
    if (JsonString(root, "status") != "1" || !cJSON_IsObject(live)) {
        ESP_LOGW(TAG, "高德实时天气响应无效: %s", JsonString(root, "info").c_str());
        if (root) cJSON_Delete(root);
        return false;
    }
    WeatherData data = latest_data_.Get();
    data.city = config.city;
    data.temp = JsonString(live, "temperature");
    data.text = JsonString(live, "weather");
    data.icon.clear(); data.feels_like = data.temp;
    data.humidity = JsonString(live, "humidity");
    data.wind = JsonString(live, "winddirection") + JsonString(live, "windpower");
    if (!data.wind.empty()) data.wind += "级";
    data.update_time = JsonString(live, "reporttime");
    data.valid = !data.temp.empty() && !data.text.empty();
    cJSON_Delete(root);
    if (data.valid) latest_data_.Set(std::move(data));
    return data.valid;
}

bool WeatherManager::parseAmapForecastJson(const char* json_data) {
    cJSON* root = cJSON_Parse(json_data);
    cJSON* forecasts = root ? cJSON_GetObjectItem(root, "forecasts") : nullptr;
    cJSON* forecast = cJSON_IsArray(forecasts) ? cJSON_GetArrayItem(forecasts, 0) : nullptr;
    cJSON* casts = forecast ? cJSON_GetObjectItem(forecast, "casts") : nullptr;
    if (JsonString(root, "status") != "1" || !cJSON_IsArray(casts) || cJSON_GetArraySize(casts) == 0) { if (root) cJSON_Delete(root); return false; }
    latest_data_.Modify([&](WeatherData& data) {
        data.forecast_count = std::min(3, cJSON_GetArraySize(casts));
        for (int index = 0; index < data.forecast_count; ++index) {
            cJSON* item = cJSON_GetArrayItem(casts, index);
            data.forecast[index] = {JsonString(item, "date"), JsonString(item, "dayweather"), "",
                                    JsonString(item, "daytemp"), JsonString(item, "nighttemp")};
        }
    });
    cJSON_Delete(root);
    ESP_LOGI(TAG, "高德天气更新成功");
    return true;
}

bool WeatherManager::parseOpenMeteoForecastJson(const char* json_data) {
    cJSON* root = cJSON_Parse(json_data);
    cJSON* daily = root ? cJSON_GetObjectItem(root, "daily") : nullptr;
    cJSON* dates = daily ? cJSON_GetObjectItem(daily, "time") : nullptr;
    cJSON* codes = daily ? cJSON_GetObjectItem(daily, "weather_code") : nullptr;
    cJSON* maximums = daily ? cJSON_GetObjectItem(daily, "temperature_2m_max") : nullptr;
    cJSON* minimums = daily ? cJSON_GetObjectItem(daily, "temperature_2m_min") : nullptr;
    constexpr int kForecastDays = 7;
    if (!cJSON_IsArray(dates) || !cJSON_IsArray(codes) ||
        !cJSON_IsArray(maximums) || !cJSON_IsArray(minimums) ||
        cJSON_GetArraySize(dates) < kForecastDays ||
        cJSON_GetArraySize(codes) < kForecastDays ||
        cJSON_GetArraySize(maximums) < kForecastDays ||
        cJSON_GetArraySize(minimums) < kForecastDays) {
        if (root) cJSON_Delete(root);
        return false;
    }

    std::array<ForecastDay, kForecastDays> parsed;
    for (int index = 0; index < kForecastDays; ++index) {
        cJSON* date = cJSON_GetArrayItem(dates, index);
        cJSON* code = cJSON_GetArrayItem(codes, index);
        const std::string maximum = RoundedNumber(cJSON_GetArrayItem(maximums, index));
        const std::string minimum = RoundedNumber(cJSON_GetArrayItem(minimums, index));
        if (!cJSON_IsString(date) || !cJSON_IsNumber(code) || maximum.empty() || minimum.empty()) {
            cJSON_Delete(root);
            return false;
        }
        const int weather_code = code->valueint;
        parsed[index] = {date->valuestring, WmoWeatherText(weather_code),
                         std::to_string(weather_code), maximum, minimum};
    }
    cJSON_Delete(root);
    latest_data_.Modify([&](WeatherData& data) {
        data.forecast = std::move(parsed);
        data.forecast_count = 7;
    });
    ESP_LOGI(TAG, "Open-Meteo 七日天气更新成功");
    return true;
}
