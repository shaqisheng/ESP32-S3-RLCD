#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <esp_http_server.h>

class AdminServer {
public:
    static AdminServer& GetInstance();
    bool Start();

private:
    httpd_handle_t server_ = nullptr;
    std::mutex session_mutex_;
    std::string session_id_;
    std::string csrf_token_;
    int64_t session_seen_sec_ = 0;          // 上次活动墙上时钟（epoch 秒）
    int64_t session_last_persisted_us_ = 0; // 上次写 NVS 的 boot 微秒，限频用

    AdminServer() = default;
    bool HasPassword() const;
    bool SetPassword(const std::string& password);
    bool CheckPassword(const std::string& password) const;
    bool IsAuthorized(httpd_req_t* req, bool csrf);
    bool IsApiAuthorized(httpd_req_t* req, bool csrf);
    std::string GetApiToken(bool regenerate = false);
    void CreateSession(std::string& sid, std::string& csrf);
    bool LoadSession();          // 启动时从 NVS 还原会话（跨烧录保留）
    void SaveSession();          // 把当前会话写入 NVS（调用方需持锁）
    void ClearPersistedSession();  // 清掉 NVS 里的会话

    static esp_err_t PageHandler(httpd_req_t* req);
    static esp_err_t SetupHandler(httpd_req_t* req);
    static esp_err_t LoginHandler(httpd_req_t* req);
    static esp_err_t LogoutHandler(httpd_req_t* req);
    static esp_err_t StatusHandler(httpd_req_t* req);
    static esp_err_t PagesGetHandler(httpd_req_t* req);
    static esp_err_t PagesPutHandler(httpd_req_t* req);
    static esp_err_t QuotasGetHandler(httpd_req_t* req);
    static esp_err_t QuotasPutHandler(httpd_req_t* req);
    static esp_err_t RefreshHandler(httpd_req_t* req);
    static esp_err_t RefreshIntervalGetHandler(httpd_req_t* req);
    static esp_err_t RefreshIntervalPutHandler(httpd_req_t* req);
    static esp_err_t ProxyDiagnosticHandler(httpd_req_t* req);
    static esp_err_t TodosHandler(httpd_req_t* req);
    static esp_err_t TodoItemHandler(httpd_req_t* req);
    static esp_err_t WeatherGetHandler(httpd_req_t* req);
    static esp_err_t WeatherPutHandler(httpd_req_t* req);
    static esp_err_t WeatherDiagnosticHandler(httpd_req_t* req);
    static esp_err_t CalendarGetHandler(httpd_req_t* req);
    static esp_err_t CalendarPutHandler(httpd_req_t* req);
    static esp_err_t CalendarSyncHandler(httpd_req_t* req);
    static esp_err_t ApiTokenHandler(httpd_req_t* req);
    static esp_err_t DeviceHandler(httpd_req_t* req);
    static esp_err_t DisplaySwitchHandler(httpd_req_t* req);
    static esp_err_t ScreenshotHandler(httpd_req_t* req);
    static esp_err_t WifiListHandler(httpd_req_t* req);
    static esp_err_t WifiAddHandler(httpd_req_t* req);
    static esp_err_t WifiDeleteHandler(httpd_req_t* req);
    static esp_err_t WifiDefaultHandler(httpd_req_t* req);
    static esp_err_t WifiConnectHandler(httpd_req_t* req);
    static esp_err_t WifiDisconnectHandler(httpd_req_t* req);
    static esp_err_t WeatherRefreshHandler(httpd_req_t* req);
    static esp_err_t WifiApStartHandler(httpd_req_t* req);
    static esp_err_t WifiApStopHandler(httpd_req_t* req);
    static esp_err_t QuotaDisplayHandler(httpd_req_t* req);
    static esp_err_t QuotaRefreshOneHandler(httpd_req_t* req);
    static esp_err_t PowerSaveHandler(httpd_req_t* req);
    static esp_err_t LogsHandler(httpd_req_t* req);
};
