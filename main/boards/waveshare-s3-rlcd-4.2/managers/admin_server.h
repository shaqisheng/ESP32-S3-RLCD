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
    int64_t session_seen_us_ = 0;

    AdminServer() = default;
    bool HasPassword() const;
    bool SetPassword(const std::string& password);
    bool CheckPassword(const std::string& password) const;
    bool IsAuthorized(httpd_req_t* req, bool csrf);
    bool IsApiAuthorized(httpd_req_t* req, bool csrf);
    std::string GetApiToken(bool regenerate = false);
    void CreateSession(std::string& sid, std::string& csrf);

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
};
