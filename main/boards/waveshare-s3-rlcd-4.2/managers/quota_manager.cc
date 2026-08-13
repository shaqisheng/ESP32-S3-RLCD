#include "quota_manager.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "application.h"
#include "manager_safety.h"
#include "quota_proxy_transport.h"
#include "proxy_auth.h"
#include "wifi_manager.h"

namespace {
constexpr const char* TAG = "QuotaManager";
constexpr const char* kPartition = "quota_nvs";
constexpr const char* kNamespace = "quota";
constexpr size_t kMaxEntries = 32;
constexpr size_t kMaxBody = 16 * 1024;
constexpr uint32_t kDefaultRefreshMinutes = 5;
constexpr uint32_t kMinRefreshMinutes = 1;
constexpr uint32_t kMaxRefreshMinutes = 60;
constexpr uint32_t kNetworkIdleGuardMs = 30000;

struct HttpBuffer {
    char* data = nullptr;
    size_t len = 0;
    bool overflow = false;
    uint32_t generation = 0;
};

esp_err_t OnHttpEvent(esp_http_client_event_t* evt) {
    if (!evt->user_data) return ESP_OK;
    auto* out = static_cast<HttpBuffer*>(evt->user_data);
    if (rlcd::BackgroundNetworkCancelled(out->generation)) return ESP_FAIL;
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (!out->data || out->len + evt->data_len >= kMaxBody) {
        out->overflow = true;
        return ESP_FAIL;
    }
    memcpy(out->data + out->len, evt->data, evt->data_len);
    out->len += evt->data_len;
    out->data[out->len] = '\0';
    return ESP_OK;
}

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

// 解析 ISO 8601 字符串（如 "2026-08-11T15:53:05.519605Z"）为 Unix 秒（UTC）。
// 用 Howard Hinnant 的 days_from_civil 算法手算，避免依赖 timegm。
// 解析失败返回 0。Kimi 的 resetTime 字段用这种格式。
int64_t ParseIso8601ToUnix(const std::string& iso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    if (mo < 1 || mo > 12 || y < 1970 || y > 2100) return 0;
    int yi = y - (mo <= 2);
    const int era = (yi >= 0 ? yi : yi - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yi - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int days = era * 146097 + static_cast<int>(doe) - 719468;
    return static_cast<int64_t>(days) * 86400 + h * 3600 + mi * 60 + s;
}

// 解析"重置时间"字段，兼容三种返回格式：
//   1. JSON 数字（Unix 毫秒，如 GLM 的 nextResetTime）→ /1000
//   2. ISO 8601 字符串（如 Kimi 的 resetTime "2026-08-11T15:53:05Z"）→ 直接解析
//   3. 数字字符串（防御性，"1748523456000"）→ atof + /1000
// 返回 Unix 秒（UTC）或 0（缺失/无效）。
int64_t ParseResetAt(cJSON* obj, const char* key) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!value) return 0;
    if (cJSON_IsNumber(value)) {
        return static_cast<int64_t>(value->valuedouble / 1000.0);
    }
    if (cJSON_IsString(value) && value->valuestring && value->valuestring[0]) {
        const std::string s = value->valuestring;
        if (s.size() >= 10 && s[4] == '-' && s[7] == '-') {
            return ParseIso8601ToUnix(s);
        }
        const double n = atof(s.c_str());
        if (n > 0) return static_cast<int64_t>(n / 1000.0);
    }
    return 0;
}

bool JsonBool(cJSON* obj, const char* key, bool fallback) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(value) ? cJSON_IsTrue(value) : fallback;
}

int ClampPercent(double value) {
    return std::max(0, std::min(100, static_cast<int>(lround(value))));
}

std::string ProviderLabel(const std::string& provider) {
    if (provider == "codex") return "Codex";
    if (provider == "kimi") return "Kimi";
    if (provider == "glm-cn" || provider == "glm-global") return "GLM";
    if (provider == "deepseek") return "DeepSeek";
    if (provider == "manual") return "手动";
    return "JSON";
}

bool IsProvider(const std::string& provider) {
    static const char* allowed[] = {"codex", "kimi", "glm-cn", "glm-global",
                                    "deepseek", "generic-json", "manual"};
    for (auto* item : allowed) if (provider == item) return true;
    return false;
}

void AddTierJson(cJSON* array, const QuotaTier& tier) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "label", tier.label.c_str());
    cJSON_AddNumberToObject(item, "used_percent", tier.used_percent);
    cJSON_AddNumberToObject(item, "total", tier.total);
    cJSON_AddNumberToObject(item, "remaining", tier.remaining);
    cJSON_AddStringToObject(item, "unit", tier.unit.c_str());
    cJSON_AddNumberToObject(item, "reset_at", static_cast<double>(tier.reset_at));
    cJSON_AddItemToArray(array, item);
}

QuotaTier ParseTierJson(cJSON* item) {
    QuotaTier tier;
    tier.label = JsonString(item, "label");
    tier.used_percent = static_cast<int>(JsonNumber(item, "used_percent", -1));
    tier.total = JsonNumber(item, "total");
    tier.remaining = JsonNumber(item, "remaining");
    tier.unit = JsonString(item, "unit");
    tier.reset_at = static_cast<int64_t>(JsonNumber(item, "reset_at"));
    return tier;
}
}  // namespace

QuotaManager& QuotaManager::GetInstance() {
    static QuotaManager instance;
    return instance;
}

bool QuotaManager::Init() {
    if (initialized_) return true;
    esp_err_t err = nvs_flash_init_partition(kPartition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase_partition(kPartition));
        err = nvs_flash_init_partition(kPartition);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "额度 NVS 初始化失败: %s", esp_err_to_name(err));
        return false;
    }
    Load();
    initialized_ = true;
    ESP_LOGI(TAG, "额度管理初始化完成，%u 个配置项", static_cast<unsigned>(entries_.size()));
    return true;
}

void QuotaManager::Start() {
    if (!initialized_ || task_) return;
    xTaskCreate(TaskMain, "quota_refresh", 12288, this, 2, &task_);
}

void QuotaManager::TaskMain(void* arg) {
    static_cast<QuotaManager*>(arg)->Run();
}

void QuotaManager::Run() {
    uint32_t last_cycle = 0;
    uint32_t idle_since_ms = 0;
    uint32_t idle_generation = 0;
    vTaskDelay(pdMS_TO_TICKS(1000));
    while (true) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t refresh_minutes = kDefaultRefreshMinutes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            refresh_minutes = refresh_interval_minutes_;
        }
        const uint32_t refresh_ms = refresh_minutes * 60 * 1000;
        const bool requested = refresh_requested_.load();
        const bool due = last_cycle == 0 || now - last_cycle >= refresh_ms || requested;
        auto state = Application::GetInstance().GetDeviceState();
        const bool idle = state == kDeviceStateIdle && WifiManager::GetInstance().IsConnected();
        if (!idle) {
            idle_since_ms = 0;
        } else if (idle_since_ms == 0) {
            idle_since_ms = now;
            idle_generation = rlcd::BackgroundNetworkGeneration();
        }
        const uint32_t current_generation = rlcd::BackgroundNetworkGeneration();
        const bool stable_idle = idle_since_ms > 0 &&
                                 now - idle_since_ms >= kNetworkIdleGuardMs &&
                                 idle_generation == current_generation;
        if (idle_since_ms > 0 && idle_generation != current_generation) {
            idle_since_ms = now;
            idle_generation = current_generation;
        }
        if (due && stable_idle) {
            refresh_requested_.store(false);
            RefreshAll();
            last_cycle = xTaskGetTickCount() * portTICK_PERIOD_MS;
            idle_since_ms = last_cycle;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void QuotaManager::RequestRefresh() {
    refresh_requested_ = true;
}

std::vector<QuotaCard> QuotaManager::GetCards() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QuotaCard> result;
    for (const auto& card : cards_) if (card.enabled) result.push_back(card);
    return result;
}

std::vector<QuotaPageSetting> QuotaManager::GetPageSettings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pages_;
}

int64_t QuotaManager::GetLastAllSuccessAt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_all_success_at_;
}

uint32_t QuotaManager::GetRefreshIntervalMinutes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return refresh_interval_minutes_;
}

bool QuotaManager::SetRefreshIntervalMinutes(uint32_t minutes, std::string& error) {
    if (minutes < kMinRefreshMinutes || minutes > kMaxRefreshMinutes) {
        error = "刷新间隔必须为 1-60 分钟";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        error = "打开额度配置失败";
        return false;
    }
    const esp_err_t result = nvs_set_u32(handle, "refresh_min", minutes) == ESP_OK
        ? nvs_commit(handle) : ESP_FAIL;
    nvs_close(handle);
    if (result != ESP_OK) { error = "保存刷新间隔失败"; return false; }
    refresh_interval_minutes_ = minutes;
    refresh_requested_ = true;
    revision_++;
    return true;
}

bool QuotaManager::SetDisplayConfig(uint8_t cards_per_page, uint8_t auto_advance_seconds,
                                    uint8_t force_page, std::string& error) {
    if (cards_per_page < 1 || cards_per_page > 4) { error = "每屏卡片数必须为 1-4"; return false; }
    if (auto_advance_seconds > 120) { error = "自动翻页间隔不能超过 120 秒（0=不翻页）"; return false; }
    if (force_page > 32) { error = "固定页码不能超过 32"; return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        error = "打开配置失败";
        return false;
    }
    esp_err_t err = nvs_set_u8(handle, "cards_per_page", cards_per_page);
    if (err == ESP_OK) err = nvs_set_u8(handle, "auto_adv_sec", auto_advance_seconds);
    if (err == ESP_OK) err = nvs_set_u8(handle, "force_page", force_page);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) { error = "保存显示配置失败"; return false; }
    cards_per_page_ = cards_per_page;
    auto_advance_seconds_ = auto_advance_seconds;
    force_page_ = force_page;
    revision_++;
    return true;
}

bool QuotaManager::HttpGet(const Entry& entry, const std::string& url, std::string& body,
                           int& status, std::string& error) {
    auto* raw = static_cast<char*>(heap_caps_malloc(kMaxBody, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!raw) raw = static_cast<char*>(heap_caps_malloc(kMaxBody, MALLOC_CAP_8BIT));
    if (!raw) { error = "内存不足"; return false; }
    raw[0] = '\0';
    HttpBuffer output{raw, 0, false, rlcd::BackgroundNetworkGeneration()};
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.event_handler = OnHttpEvent;
    cfg.user_data = &output;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    esp_transport_handle_t proxy_transport = nullptr;
    if (entry.proxy_enabled) {
        proxy_transport = CreateQuotaProxyTransport(entry.proxy_url, url.rfind("https://", 0) == 0, error);
        if (!proxy_transport) { heap_caps_free(raw); return false; }
        cfg.transport = proxy_transport;
    }
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (proxy_transport) esp_transport_destroy(proxy_transport);
        heap_caps_free(raw); error = "HTTP 初始化失败"; return false;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    if (entry.provider == "codex") {
        std::string auth = "Bearer " + entry.secret;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
        esp_http_client_set_header(client, "User-Agent", "codex-cli");
        if (!entry.account_id.empty())
            esp_http_client_set_header(client, "ChatGPT-Account-Id", entry.account_id.c_str());
    } else if (entry.provider == "glm-cn" || entry.provider == "glm-global") {
        esp_http_client_set_header(client, "Authorization", entry.secret.c_str());
        esp_http_client_set_header(client, "Accept-Language", "zh-CN,zh");
    } else if (!entry.secret.empty()) {
        std::string auth = "Bearer " + entry.secret;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    esp_err_t err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    const std::string proxy_error = proxy_transport
        ? GetQuotaProxyTransportError(proxy_transport) : "";
    esp_http_client_cleanup(client);
    if (proxy_transport) esp_transport_destroy(proxy_transport);
    if (err != ESP_OK || output.overflow) {
        error = output.overflow ? "响应超过 16KB" :
                (entry.proxy_enabled
                    ? (proxy_error.empty() ? "代理请求失败" : proxy_error)
                    : esp_err_to_name(err));
        heap_caps_free(raw);
        return false;
    }
    body.assign(raw, output.len);
    heap_caps_free(raw);
    return true;
}

bool QuotaManager::ParseResponse(const Entry& entry, const char* json, QuotaCard& card,
                                 std::string& error) {
    cJSON* root = cJSON_Parse(json);
    if (!root) { error = "JSON 解析失败"; return false; }
    auto finish = [&](bool ok) { cJSON_Delete(root); return ok; };

    if (entry.provider == "codex") {
        cJSON* rate = cJSON_GetObjectItem(root, "rate_limit");
        if (!cJSON_IsObject(rate)) { error = "缺少 rate_limit"; return finish(false); }
        const char* keys[] = {"primary_window", "secondary_window"};
        for (auto* key : keys) {
            cJSON* window = cJSON_GetObjectItem(rate, key);
            if (!cJSON_IsObject(window)) continue;
            int seconds = static_cast<int>(JsonNumber(window, "limit_window_seconds"));
            QuotaTier tier;
            tier.label = seconds == 18000 ? "5H" : (seconds == 604800 ? "7D" : "窗口");
            tier.used_percent = ClampPercent(JsonNumber(window, "used_percent"));
            tier.total = 100;
            tier.remaining = 100 - tier.used_percent;
            tier.unit = "%";
            tier.reset_at = static_cast<int64_t>(JsonNumber(window, "reset_at"));
            card.tiers.push_back(tier);
        }
    } else if (entry.provider == "kimi") {
        cJSON* limits = cJSON_GetObjectItem(root, "limits");
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, limits) {
            if (card.tiers.size() >= 1) break;
            cJSON* detail = cJSON_GetObjectItem(item, "detail");
            if (!cJSON_IsObject(detail)) continue;
            QuotaTier tier;
            tier.label = "5H";
            tier.total = JsonNumber(detail, "limit");
            tier.remaining = JsonNumber(detail, "remaining");
            tier.used_percent = tier.total > 0 ? ClampPercent((tier.total - tier.remaining) * 100 / tier.total) : -1;
            tier.reset_at = ParseResetAt(detail, "resetTime");
            card.tiers.push_back(tier);
        }
        cJSON* usage = cJSON_GetObjectItem(root, "usage");
        if (cJSON_IsObject(usage)) {
            QuotaTier tier;
            tier.label = "周";
            tier.total = JsonNumber(usage, "limit");
            tier.remaining = JsonNumber(usage, "remaining");
            tier.used_percent = tier.total > 0 ? ClampPercent((tier.total - tier.remaining) * 100 / tier.total) : -1;
            tier.reset_at = ParseResetAt(usage, "resetTime");
            card.tiers.push_back(tier);
        }
    } else if (entry.provider == "glm-cn" || entry.provider == "glm-global") {
        cJSON* data = cJSON_GetObjectItem(root, "data");
        cJSON* limits = data ? cJSON_GetObjectItem(data, "limits") : nullptr;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, limits) {
            if (card.tiers.size() >= 2 || JsonString(item, "type") != "TOKENS_LIMIT") continue;
            int unit = static_cast<int>(JsonNumber(item, "unit"));
            QuotaTier tier;
            tier.label = unit == 3 ? "5H" : (unit == 6 ? "周" : "额度");
            tier.used_percent = ClampPercent(JsonNumber(item, "percentage"));
            tier.total = 100;
            tier.remaining = 100 - tier.used_percent;
            tier.unit = "%";
            tier.reset_at = ParseResetAt(item, "nextResetTime");
            card.tiers.push_back(tier);
        }
    } else if (entry.provider == "deepseek") {
        cJSON* infos = cJSON_GetObjectItem(root, "balance_infos");
        cJSON* info = cJSON_IsArray(infos) ? cJSON_GetArrayItem(infos, 0) : nullptr;
        if (cJSON_IsObject(info)) {
            QuotaTier tier;
            tier.label = "余额";
            tier.remaining = JsonNumber(info, "total_balance");
            tier.unit = JsonString(info, "currency", "CNY");
            card.tiers.push_back(tier);
        }
    } else if (entry.provider == "generic-json") {
        cJSON* tiers = cJSON_GetObjectItem(root, "tiers");
        if (cJSON_IsArray(tiers)) {
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, tiers) {
                if (card.tiers.size() >= 2) break;
                card.tiers.push_back(ParseTierJson(item));
            }
        } else {
            QuotaTier tier;
            tier.label = entry.label.empty() ? "额度" : entry.label;
            tier.total = JsonNumber(root, entry.total_field.empty() ? "total" : entry.total_field.c_str());
            tier.remaining = JsonNumber(root, entry.remaining_field.empty() ? "remaining" : entry.remaining_field.c_str());
            tier.unit = entry.unit;
            tier.used_percent = tier.total > 0 ? ClampPercent((tier.total - tier.remaining) * 100 / tier.total) : -1;
            card.tiers.push_back(tier);
        }
    }
    if (card.tiers.empty()) { error = "未找到额度数据"; return finish(false); }
    return finish(true);
}

bool QuotaManager::RefreshOne(const Entry& entry, QuotaCard& card, bool& transient) {
    transient = false;
    card.checked_at = time(nullptr);
    card.tiers.clear();
    if (entry.provider == "manual") {
        QuotaTier tier;
        tier.label = entry.label.empty() ? "额度" : entry.label;
        tier.total = entry.manual_total;
        tier.remaining = entry.manual_remaining;
        tier.unit = entry.unit;
        tier.used_percent = tier.total > 0 ? ClampPercent((tier.total - tier.remaining) * 100 / tier.total) : -1;
        card.tiers.push_back(tier);
        return true;
    }
    std::string url = entry.base_url;
    if (url.empty()) {
        if (entry.provider == "codex") url = "https://chatgpt.com/backend-api/wham/usage";
        else if (entry.provider == "kimi") url = "https://api.kimi.com/coding/v1/usages";
        else if (entry.provider == "glm-cn") url = "https://open.bigmodel.cn/api/monitor/usage/quota/limit";
        else if (entry.provider == "glm-global") url = "https://api.z.ai/api/monitor/usage/quota/limit";
        else if (entry.provider == "deepseek") url = "https://api.deepseek.com/user/balance";
    }
    if (url.empty()) { card.error = "缺少 URL"; return false; }
    std::string body, error;
    int status = 0;
    if (!HttpGet(entry, url, body, status, error)) {
        transient = true;
        card.error = error;
        return false;
    }
    if (status == 401 || status == 403) { card.error = "认证失效"; return false; }
    if (status < 200 || status >= 300) { card.error = "HTTP " + std::to_string(status); return false; }
    if (!ParseResponse(entry, body.c_str(), card, error)) { card.error = error; return false; }
    return true;
}

void QuotaManager::RefreshAll() {
    rlcd::BackgroundNetworkSession network_session;
    if (network_session.cancelled()) return;
    refreshing_ = true;
    std::vector<Entry> entries;
    std::vector<QuotaCard> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries = entries_;
        previous = cards_;
    }
    std::vector<QuotaCard> next;
    bool all_ok = true;
    bool has_enabled = false;
    std::string last_provider;  // 跟踪上一个 provider，同 provider 间隔加大避免限流
    for (const auto& entry : entries) {
        if (network_session.cancelled()) {
            all_ok = false;
            break;
        }
        QuotaCard fresh;
        fresh.id = entry.id;
        fresh.name = entry.name.empty() ? ProviderLabel(entry.provider) : entry.name;
        fresh.provider = entry.provider;
        fresh.enabled = entry.enabled;
        if (!entry.enabled) { next.push_back(fresh); continue; }
        has_enabled = true;
        bool transient = false;
        bool ok = RefreshOne(entry, fresh, transient);
        // transient 错误（网络层失败）重试最多 3 次，间隔递增（2s/4s/8s）
        // 应对同代理同目标的偶发 socket/TLS 失败
        for (int attempt = 1; !ok && transient && attempt <= 3 && !network_session.cancelled(); ++attempt) {
            const uint32_t backoff_ms = 1000 * (1 << attempt);  // 2s, 4s, 8s
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            QuotaCard retry_card;
            retry_card.id = entry.id;
            retry_card.name = fresh.name;
            retry_card.provider = entry.provider;
            retry_card.enabled = entry.enabled;
            bool retry_transient = false;
            if (RefreshOne(entry, retry_card, retry_transient)) {
                fresh = retry_card;
                ok = true;
            } else {
                transient = retry_transient;  // 只有 transient 才继续重试
            }
        }
        if (ok) {
            fresh.valid = true;
            fresh.stale = false;
            fresh.success_at = fresh.checked_at;
            SaveCardCache(fresh);
        } else {
            all_ok = false;
            auto old = std::find_if(previous.begin(), previous.end(), [&](const QuotaCard& c) { return c.id == entry.id; });
            if (old != previous.end() && old->valid) {
                std::string latest_error = fresh.error;
                int64_t checked = fresh.checked_at;
                fresh = *old;
                fresh.error = latest_error;
                fresh.checked_at = checked;
                fresh.stale = true;
            }
        }
        next.push_back(fresh);
        // 同 provider（尤其同代理+同目标）间隔加大到 1.5 秒避免限流；
        // 不同 provider 保持 150ms 快速串行
        const uint32_t delay_ms = (entry.provider == last_provider) ? 1500 : 150;
        last_provider = entry.provider;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cards_ = std::move(next);
        if (all_ok && has_enabled) last_all_success_at_ = time(nullptr);
        last_refresh_completed_at_ = time(nullptr);  // 刷新完成就更新（不管成败）
        revision_++;
    }
    if (all_ok && has_enabled) {
        nvs_handle_t handle;
        if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_i64(handle, "all_ok_at", last_all_success_at_);
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
    refreshing_ = false;
    ESP_LOGI(TAG, "额度刷新完成: %s", all_ok ? "全部成功" : "存在失败");
}

void QuotaManager::Load() {
    std::lock_guard<std::mutex> lock(mutex_);
    pages_ = {{"overview", true, 0}, {"calendar", true, 1}, {"forecast", true, 2}, {"quota", true, 3}, {"todo", true, 4}};
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint32_t count = 0;
    nvs_get_u32(handle, "count", &count);
    count = std::min<uint32_t>(count, kMaxEntries);
    for (uint32_t i = 0; i < count; ++i) {
        char key[16]; snprintf(key, sizeof(key), "e%02u", static_cast<unsigned>(i));
        size_t len = 0;
        if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK || len > 4096) continue;
        std::vector<char> buf(len);
        if (nvs_get_str(handle, key, buf.data(), &len) != ESP_OK) continue;
        cJSON* item = cJSON_Parse(buf.data());
        if (!item) continue;
        Entry e;
        e.id = static_cast<uint32_t>(JsonNumber(item, "id", i + 1));
        e.enabled = JsonBool(item, "enabled", true);
        e.order = static_cast<int>(JsonNumber(item, "order", i));
        e.name = JsonString(item, "name"); e.provider = JsonString(item, "provider");
        e.base_url = JsonString(item, "base_url");
        e.proxy_enabled = JsonBool(item, "proxy_enabled", false);
        e.proxy_url = JsonString(item, "proxy_url"); e.secret = JsonString(item, "secret");
        e.account_id = JsonString(item, "account_id"); e.unit = JsonString(item, "unit");
        e.label = JsonString(item, "label"); e.total_field = JsonString(item, "total_field");
        e.remaining_field = JsonString(item, "remaining_field");
        e.manual_total = JsonNumber(item, "manual_total");
        e.manual_remaining = JsonNumber(item, "manual_remaining");
        cJSON_Delete(item);
        if (IsProvider(e.provider)) entries_.push_back(e);
    }
    size_t pages_len = 0;
    if (nvs_get_str(handle, "pages", nullptr, &pages_len) == ESP_OK && pages_len < 1024) {
        std::vector<char> buf(pages_len);
        if (nvs_get_str(handle, "pages", buf.data(), &pages_len) == ESP_OK) {
            cJSON* array = cJSON_Parse(buf.data());
            if (cJSON_IsArray(array)) {
                std::vector<QuotaPageSetting> loaded;
                cJSON* item = nullptr;
                cJSON_ArrayForEach(item, array) {
                    loaded.push_back({JsonString(item, "id"), JsonBool(item, "enabled", true),
                                      static_cast<int>(JsonNumber(item, "order"))});
                }
                bool current_schema = loaded.size() == 4 || loaded.size() == 5;
                for (const auto& page : loaded) {
                    if (page.id != "overview" && page.id != "calendar" && page.id != "forecast" &&
                        page.id != "quota" && page.id != "todo") current_schema = false;
                }
                // 自动迁移：旧版 4 页 NVS 数据补上 todo
                if (current_schema && loaded.size() == 4) {
                    loaded.push_back({"todo", true, 4});
                }
                if (current_schema) pages_ = loaded;
            }
            if (array) cJSON_Delete(array);
        }
    }
    nvs_get_i64(handle, "all_ok_at", &last_all_success_at_);
    uint32_t saved_refresh_minutes = kDefaultRefreshMinutes;
    if (nvs_get_u32(handle, "refresh_min", &saved_refresh_minutes) == ESP_OK &&
        saved_refresh_minutes >= kMinRefreshMinutes && saved_refresh_minutes <= kMaxRefreshMinutes) {
        refresh_interval_minutes_ = saved_refresh_minutes;
    }
    // AI 页显示配置（缺省用字段默认值，有 NVS 则覆盖）
    uint8_t v = 0;
    if (nvs_get_u8(handle, "cards_per_page", &v) == ESP_OK && v >= 1 && v <= 4) cards_per_page_ = v;
    if (nvs_get_u8(handle, "auto_adv_sec", &v) == ESP_OK && v <= 120) auto_advance_seconds_ = v;
    if (nvs_get_u8(handle, "force_page", &v) == ESP_OK && v <= 32) force_page_ = v;
    nvs_close(handle);
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
    for (const auto& e : entries_) cards_.push_back({e.id, e.name.empty() ? ProviderLabel(e.provider) : e.name,
                                                     e.provider, e.enabled});
    LoadCacheLocked();
}

void QuotaManager::LoadCacheLocked() {
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    for (auto& card : cards_) {
        char key[12]; snprintf(key, sizeof(key), "r%08x", static_cast<unsigned>(card.id));
        size_t len = 0;
        if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK || len > 2048) continue;
        std::vector<char> buf(len);
        if (nvs_get_str(handle, key, buf.data(), &len) != ESP_OK) continue;
        cJSON* root = cJSON_Parse(buf.data());
        if (!root) continue;
        card.success_at = static_cast<int64_t>(JsonNumber(root, "success_at"));
        card.checked_at = card.success_at; card.valid = true; card.stale = true;
        cJSON* tiers = cJSON_GetObjectItem(root, "tiers"); cJSON* item = nullptr;
        cJSON_ArrayForEach(item, tiers) card.tiers.push_back(ParseTierJson(item));
        cJSON_Delete(root);
    }
    nvs_close(handle);
}

void QuotaManager::SaveCardCache(const QuotaCard& card) {
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "success_at", static_cast<double>(card.success_at));
    cJSON* tiers = cJSON_AddArrayToObject(root, "tiers");
    for (const auto& tier : card.tiers) AddTierJson(tiers, tier);
    char* json = cJSON_PrintUnformatted(root);
    char key[12]; snprintf(key, sizeof(key), "r%08x", static_cast<unsigned>(card.id));
    if (json) nvs_set_str(handle, key, json);
    nvs_commit(handle); nvs_close(handle);
    if (json) cJSON_free(json);
    cJSON_Delete(root);
}

bool QuotaManager::SaveEntriesLocked(std::string& error) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) { error = esp_err_to_name(err); return false; }
    nvs_set_u32(handle, "count", entries_.size());
    for (size_t i = 0; i < kMaxEntries; ++i) {
        char key[16]; snprintf(key, sizeof(key), "e%02u", static_cast<unsigned>(i));
        if (i >= entries_.size()) { nvs_erase_key(handle, key); continue; }
        const auto& e = entries_[i];
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", e.id); cJSON_AddBoolToObject(item, "enabled", e.enabled);
        cJSON_AddNumberToObject(item, "order", e.order); cJSON_AddStringToObject(item, "name", e.name.c_str());
        cJSON_AddStringToObject(item, "provider", e.provider.c_str());
        cJSON_AddStringToObject(item, "base_url", e.base_url.c_str());
        cJSON_AddBoolToObject(item, "proxy_enabled", e.proxy_enabled);
        cJSON_AddStringToObject(item, "proxy_url", e.proxy_url.c_str());
        cJSON_AddStringToObject(item, "secret", e.secret.c_str());
        cJSON_AddStringToObject(item, "account_id", e.account_id.c_str());
        cJSON_AddStringToObject(item, "unit", e.unit.c_str()); cJSON_AddStringToObject(item, "label", e.label.c_str());
        cJSON_AddStringToObject(item, "total_field", e.total_field.c_str());
        cJSON_AddStringToObject(item, "remaining_field", e.remaining_field.c_str());
        cJSON_AddNumberToObject(item, "manual_total", e.manual_total);
        cJSON_AddNumberToObject(item, "manual_remaining", e.manual_remaining);
        char* json = cJSON_PrintUnformatted(item);
        if (!json || strlen(json) >= 4000 || nvs_set_str(handle, key, json) != ESP_OK) err = ESP_FAIL;
        if (json) cJSON_free(json);
        cJSON_Delete(item);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) { error = "保存额度配置失败"; return false; }
    return true;
}

bool QuotaManager::SavePagesLocked(std::string& error) {
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        error = "打开配置分区失败"; return false;
    }
    cJSON* array = cJSON_CreateArray();
    for (const auto& page : pages_) {
        cJSON* item = cJSON_CreateObject(); cJSON_AddStringToObject(item, "id", page.id.c_str());
        cJSON_AddBoolToObject(item, "enabled", page.enabled); cJSON_AddNumberToObject(item, "order", page.order);
        cJSON_AddItemToArray(array, item);
    }
    char* json = cJSON_PrintUnformatted(array);
    esp_err_t err = json ? nvs_set_str(handle, "pages", json) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle); if (json) cJSON_free(json); cJSON_Delete(array);
    if (err != ESP_OK) { error = "保存页面配置失败"; return false; }
    return true;
}

std::string QuotaManager::GetConfigJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject(); cJSON* array = cJSON_AddArrayToObject(root, "items");
    for (const auto& e : entries_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", e.id); cJSON_AddBoolToObject(item, "enabled", e.enabled);
        cJSON_AddNumberToObject(item, "order", e.order); cJSON_AddStringToObject(item, "name", e.name.c_str());
        cJSON_AddStringToObject(item, "provider", e.provider.c_str()); cJSON_AddStringToObject(item, "base_url", e.base_url.c_str());
        cJSON_AddBoolToObject(item, "proxy_enabled", e.proxy_enabled);
        rlcd::ProxyUrl parsed_proxy;
        std::string proxy_error;
        const bool valid_proxy = rlcd::ParseProxyUrl(e.proxy_url, parsed_proxy, proxy_error);
        const bool has_proxy_auth = valid_proxy && parsed_proxy.HasAuthentication();
        const std::string proxy_endpoint = valid_proxy ? rlcd::ProxyEndpoint(parsed_proxy) : "";
        cJSON_AddStringToObject(item, "proxy_url", has_proxy_auth ? "" : proxy_endpoint.c_str());
        cJSON_AddStringToObject(item, "proxy_endpoint", proxy_endpoint.c_str());
        cJSON_AddBoolToObject(item, "has_proxy_auth", has_proxy_auth);
        cJSON_AddBoolToObject(item, "has_secret", !e.secret.empty()); cJSON_AddStringToObject(item, "account_id", e.account_id.c_str());
        cJSON_AddStringToObject(item, "unit", e.unit.c_str()); cJSON_AddStringToObject(item, "label", e.label.c_str());
        cJSON_AddStringToObject(item, "total_field", e.total_field.c_str()); cJSON_AddStringToObject(item, "remaining_field", e.remaining_field.c_str());
        cJSON_AddNumberToObject(item, "manual_total", e.manual_total); cJSON_AddNumberToObject(item, "manual_remaining", e.manual_remaining);
        auto card = std::find_if(cards_.begin(), cards_.end(), [&](const QuotaCard& value) { return value.id == e.id; });
        if (!e.enabled) cJSON_AddStringToObject(item, "status", "disabled");
        else if (card == cards_.end() || (!card->valid && card->error.empty())) cJSON_AddStringToObject(item, "status", "pending");
        else if (card->stale) cJSON_AddStringToObject(item, "status", "stale");
        else if (!card->error.empty()) cJSON_AddStringToObject(item, "status", "error");
        else cJSON_AddStringToObject(item, "status", "ok");
        if (card != cards_.end()) {
            cJSON_AddStringToObject(item, "error", card->error.c_str());
            cJSON_AddNumberToObject(item, "checked_at", static_cast<double>(card->checked_at));
        }
        cJSON_AddItemToArray(array, item);
    }
    // AI 页显示配置（不影响既有前端 items 解析）
    cJSON_AddNumberToObject(root, "cards_per_page", cards_per_page_);
    cJSON_AddNumberToObject(root, "auto_advance_seconds", auto_advance_seconds_);
    cJSON_AddNumberToObject(root, "force_page", force_page_);
    char* json = cJSON_PrintUnformatted(root); std::string out = json ? json : "{\"items\":[]}";
    if (json) cJSON_free(json);
    cJSON_Delete(root);
    return out;
}

std::string QuotaManager::GetProxyDiagnosticJson() const {
    std::string proxy_url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto entry = std::find_if(entries_.begin(), entries_.end(), [](const Entry& value) {
            return value.enabled && value.proxy_enabled && !value.proxy_url.empty();
        });
        if (entry == entries_.end()) return "{\"configured\":false,\"stage\":\"配置\",\"message\":\"未找到启用的代理配置\"}";
        proxy_url = entry->proxy_url;
    }
    return DiagnoseQuotaProxy(proxy_url);
}

bool QuotaManager::ApplyConfigJson(const char* json, std::string& error) {
    cJSON* root = cJSON_Parse(json); cJSON* array = root ? cJSON_GetObjectItem(root, "items") : nullptr;
    if (!cJSON_IsArray(array)) { if (root) cJSON_Delete(root); error = "items 必须是数组"; return false; }
    int count = cJSON_GetArraySize(array);
    if (count < 0 || count > static_cast<int>(kMaxEntries)) { cJSON_Delete(root); error = "最多 32 项"; return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Entry> next;
    cJSON* item = nullptr; int index = 0;
    cJSON_ArrayForEach(item, array) {
        Entry e; e.id = static_cast<uint32_t>(JsonNumber(item, "id"));
        if (!e.id) e.id = static_cast<uint32_t>(esp_random());
        e.enabled = JsonBool(item, "enabled", true); e.order = index++;
        e.name = JsonString(item, "name"); e.provider = JsonString(item, "provider");
        if (e.name.empty() || e.name.size() > 32 || !IsProvider(e.provider)) { cJSON_Delete(root); error = "名称或供应商无效"; return false; }
        e.base_url = JsonString(item, "base_url"); e.account_id = JsonString(item, "account_id");
        e.proxy_enabled = JsonBool(item, "proxy_enabled", false);
        e.unit = JsonString(item, "unit"); e.label = JsonString(item, "label");
        e.total_field = JsonString(item, "total_field"); e.remaining_field = JsonString(item, "remaining_field");
        e.manual_total = JsonNumber(item, "manual_total"); e.manual_remaining = JsonNumber(item, "manual_remaining");
        std::string incoming = JsonString(item, "secret");
        bool clear = JsonBool(item, "clear_secret", false);
        auto old = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& old_e) { return old_e.id == e.id; });
        if (!incoming.empty()) e.secret = incoming; else if (!clear && old != entries_.end()) e.secret = old->secret;
        const std::string incoming_proxy = JsonString(item, "proxy_url");
        const bool clear_proxy = JsonBool(item, "clear_proxy", false);
        if (!incoming_proxy.empty()) e.proxy_url = incoming_proxy;
        else if (!clear_proxy && old != entries_.end()) e.proxy_url = old->proxy_url;
        if (e.secret.size() > 3500 || e.base_url.size() > 256 || e.account_id.size() > 128 || e.proxy_url.size() > 256) { cJSON_Delete(root); error = "字段过长"; return false; }
        if (e.proxy_enabled) {
            std::string proxy_error;
            esp_transport_handle_t validation = CreateQuotaProxyTransport(e.proxy_url, true, proxy_error);
            if (!validation) { cJSON_Delete(root); error = e.name + " " + proxy_error; return false; }
            esp_transport_destroy(validation);
        }
        if (e.provider != "manual" && e.provider != "generic-json" && e.secret.empty()) { cJSON_Delete(root); error = e.name + " 缺少凭据"; return false; }
        if (e.provider == "generic-json" && (e.base_url.rfind("http://", 0) != 0 && e.base_url.rfind("https://", 0) != 0)) {
            cJSON_Delete(root); error = e.name + " URL 必须以 http:// 或 https:// 开头"; return false;
        }
        next.push_back(std::move(e));
    }
    cJSON_Delete(root); entries_ = std::move(next);
    auto previous_cards = cards_;
    cards_.clear();
    for (const auto& e : entries_) {
        auto old = std::find_if(previous_cards.begin(), previous_cards.end(),
                                [&](const QuotaCard& card) { return card.id == e.id; });
        if (old != previous_cards.end()) {
            old->name = e.name;
            old->provider = e.provider;
            old->enabled = e.enabled;
            cards_.push_back(*old);
        } else {
            cards_.push_back({e.id, e.name, e.provider, e.enabled});
        }
    }
    if (!SaveEntriesLocked(error)) return false;
    revision_++; refresh_requested_ = true; return true;
}

std::string QuotaManager::GetPageConfigJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject(); cJSON* array = cJSON_AddArrayToObject(root, "pages");
    for (const auto& p : pages_) { cJSON* item = cJSON_CreateObject(); cJSON_AddStringToObject(item, "id", p.id.c_str());
        cJSON_AddBoolToObject(item, "enabled", p.enabled); cJSON_AddNumberToObject(item, "order", p.order); cJSON_AddItemToArray(array, item); }
    char* json = cJSON_PrintUnformatted(root); std::string out = json ? json : "{}";
    if (json) cJSON_free(json);
    cJSON_Delete(root);
    return out;
}

bool QuotaManager::ApplyPageConfigJson(const char* json, std::string& error) {
    cJSON* root = cJSON_Parse(json); cJSON* array = root ? cJSON_GetObjectItem(root, "pages") : nullptr;
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != 5) { if (root) cJSON_Delete(root); error = "页面配置必须包含 5 项"; return false; }
    std::vector<QuotaPageSetting> next; bool any = false; cJSON* item = nullptr; int order = 0;
    cJSON_ArrayForEach(item, array) { std::string id = JsonString(item, "id");
        if (id != "overview" && id != "calendar" && id != "forecast" && id != "quota" && id != "todo") { cJSON_Delete(root); error = "未知页面"; return false; }
        bool enabled = JsonBool(item, "enabled", true); any |= enabled; next.push_back({id, enabled, order++}); }
    cJSON_Delete(root); if (!any) { error = "至少启用一个页面"; return false; }
    std::lock_guard<std::mutex> lock(mutex_); pages_ = std::move(next);
    if (!SavePagesLocked(error)) return false;
    revision_++;
    return true;
}

std::string QuotaManager::GetStatusJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject(); cJSON_AddBoolToObject(root, "refreshing", refreshing_.load());
    cJSON_AddNumberToObject(root, "last_all_success_at", static_cast<double>(last_all_success_at_));
    cJSON_AddNumberToObject(root, "last_refresh_completed_at", static_cast<double>(last_refresh_completed_at_));
    cJSON_AddNumberToObject(root, "refresh_interval_minutes", refresh_interval_minutes_);
    cJSON_AddNumberToObject(root, "revision", revision_.load()); cJSON* array = cJSON_AddArrayToObject(root, "items");
    for (const auto& card : cards_) { cJSON* item = cJSON_CreateObject(); cJSON_AddNumberToObject(item, "id", card.id);
        cJSON_AddBoolToObject(item, "valid", card.valid); cJSON_AddBoolToObject(item, "stale", card.stale);
        cJSON_AddStringToObject(item, "error", card.error.c_str()); cJSON_AddNumberToObject(item, "checked_at", static_cast<double>(card.checked_at));
        cJSON* tiers = cJSON_AddArrayToObject(item, "tiers"); for (const auto& tier : card.tiers) AddTierJson(tiers, tier); cJSON_AddItemToArray(array, item); }
    char* json = cJSON_PrintUnformatted(root); std::string out = json ? json : "{}";
    if (json) cJSON_free(json);
    cJSON_Delete(root);
    return out;
}
