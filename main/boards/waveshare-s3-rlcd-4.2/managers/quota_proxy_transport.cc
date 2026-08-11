#include "quota_proxy_transport.h"
#include "proxy_auth.h"
#include "manager_safety.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

namespace {
constexpr const char* TAG = "QuotaProxy";
constexpr size_t kMaxConnectResponse = 2048;
constexpr int kTlsRetryCount = 2;
const char* kAlpnProtocols[] = {"http/1.1", nullptr};

struct ProxyTransport {
    std::string host;
    int port = 0;
    std::string authorization;
    std::string last_error;
    bool use_tls = true;
    bool proxy_uses_tls = false;
    mbedtls_net_context net;
    mbedtls_ssl_context proxy_ssl;
    mbedtls_ssl_config proxy_ssl_config;
    mbedtls_ctr_drbg_context proxy_ctr_drbg;
    mbedtls_entropy_context proxy_entropy;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_config;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    bool proxy_tls_ready = false;
    bool tls_ready = false;
    uint32_t generation = 0;

    ProxyTransport() {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&proxy_ssl);
        mbedtls_ssl_config_init(&proxy_ssl_config);
        mbedtls_ctr_drbg_init(&proxy_ctr_drbg);
        mbedtls_entropy_init(&proxy_entropy);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&ssl_config);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_entropy_init(&entropy);
    }

    ~ProxyTransport() {
        Close();
        mbedtls_ssl_free(&proxy_ssl);
        mbedtls_ssl_config_free(&proxy_ssl_config);
        mbedtls_ctr_drbg_free(&proxy_ctr_drbg);
        mbedtls_entropy_free(&proxy_entropy);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&ssl_config);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }

    void Close() {
        if (tls_ready) mbedtls_ssl_close_notify(&ssl);
        tls_ready = false;
        if (proxy_tls_ready) mbedtls_ssl_close_notify(&proxy_ssl);
        proxy_tls_ready = false;
        if (net.fd >= 0) mbedtls_net_free(&net);
    }

    void ResetTls() {
        mbedtls_ssl_free(&proxy_ssl);
        mbedtls_ssl_config_free(&proxy_ssl_config);
        mbedtls_ctr_drbg_free(&proxy_ctr_drbg);
        mbedtls_entropy_free(&proxy_entropy);
        mbedtls_ssl_init(&proxy_ssl);
        mbedtls_ssl_config_init(&proxy_ssl_config);
        mbedtls_ctr_drbg_init(&proxy_ctr_drbg);
        mbedtls_entropy_init(&proxy_entropy);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&ssl_config);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&ssl_config);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_entropy_init(&entropy);
    }
};

ProxyTransport* Context(esp_transport_handle_t transport) {
    return static_cast<ProxyTransport*>(esp_transport_get_context_data(transport));
}

void SetSocketTimeout(int fd, int timeout_ms) {
    timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

int ProxySocketWrite(ProxyTransport* ctx, const unsigned char* data, size_t size) {
    if (rlcd::BackgroundNetworkCancelled(ctx->generation)) return -1;
    if (ctx->proxy_tls_ready) return mbedtls_ssl_write(&ctx->proxy_ssl, data, size);
    return send(ctx->net.fd, data, size, 0);
}

int ProxySocketRead(ProxyTransport* ctx, unsigned char* data, size_t size) {
    if (rlcd::BackgroundNetworkCancelled(ctx->generation)) return -1;
    if (ctx->proxy_tls_ready) return mbedtls_ssl_read(&ctx->proxy_ssl, data, size);
    return recv(ctx->net.fd, data, size, 0);
}

bool SendAll(ProxyTransport* ctx, const char* data, size_t size) {
    while (size > 0) {
        int sent = ProxySocketWrite(ctx, reinterpret_cast<const unsigned char*>(data), size);
        if (sent == MBEDTLS_ERR_SSL_WANT_READ || sent == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

int ProxyTunnelSend(void* context, const unsigned char* data, size_t size) {
    return ProxySocketWrite(static_cast<ProxyTransport*>(context), data, size);
}

int ProxyTunnelReceive(void* context, unsigned char* data, size_t size) {
    return ProxySocketRead(static_cast<ProxyTransport*>(context), data, size);
}

bool OpenProxySocket(ProxyTransport* ctx, int timeout_ms) {
    char port[8];
    snprintf(port, sizeof(port), "%d", ctx->port);
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(ctx->host.c_str(), port, &hints, &addresses) != 0) return false;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        SetSocketTimeout(fd, timeout_ms);
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            ctx->net.fd = fd;
            freeaddrinfo(addresses);
            return true;
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    return false;
}

bool EstablishTunnel(ProxyTransport* ctx, const char* target_host, int target_port) {
    char request[768];
    const char* auth_name = ctx->authorization.empty() ? "" : "Proxy-Authorization: ";
    const char* auth_value = ctx->authorization.empty() ? "" : ctx->authorization.c_str();
    const char* auth_end = ctx->authorization.empty() ? "" : "\r\n";
    int length = snprintf(request, sizeof(request),
                          "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n"
                          "%s%s%sProxy-Connection: Keep-Alive\r\nConnection: Keep-Alive\r\n\r\n",
                          target_host, target_port, target_host, target_port,
                          auth_name, auth_value, auth_end);
    if (length <= 0 || length >= static_cast<int>(sizeof(request)) ||
        !SendAll(ctx, request, static_cast<size_t>(length))) {
        ctx->last_error = "代理 CONNECT 请求发送失败";
        return false;
    }

    std::string response;
    response.reserve(256);
    char byte = 0;
    while (response.size() < kMaxConnectResponse) {
        int received = ProxySocketRead(ctx, reinterpret_cast<unsigned char*>(&byte), 1);
        if (received == MBEDTLS_ERR_SSL_WANT_READ || received == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (received != 1) {
            ctx->last_error = "代理 CONNECT 响应中断";
            return false;
        }
        response.push_back(byte);
        if (response.size() >= 4 && response.compare(response.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    auto end = response.find("\r\n\r\n");
    auto first_line = response.find("\r\n");
    if (end == std::string::npos || first_line == std::string::npos) {
        ctx->last_error = "代理 CONNECT 响应格式无效";
        return false;
    }
    std::string status = response.substr(0, first_line);
    bool ok = status.rfind("HTTP/1.", 0) == 0 && status.find(" 200 ") != std::string::npos;
    if (!ok) {
        ctx->last_error = status.find(" 407 ") != std::string::npos
            ? "代理认证失败（HTTP 407）"
            : "代理拒绝 CONNECT（" + status + "）";
        ESP_LOGW(TAG, "%s", ctx->last_error.c_str());
    }
    return ok;
}

bool StartProxyTls(ProxyTransport* ctx) {
    static constexpr char kPersonalization[] = "rlcd-https-proxy";
    int result = mbedtls_ctr_drbg_seed(&ctx->proxy_ctr_drbg, mbedtls_entropy_func, &ctx->proxy_entropy,
                                      reinterpret_cast<const unsigned char*>(kPersonalization),
                                      sizeof(kPersonalization) - 1);
    if (result != 0) { ctx->last_error = "代理 TLS 随机数初始化失败"; return false; }
    result = mbedtls_ssl_config_defaults(&ctx->proxy_ssl_config, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) { ctx->last_error = "代理 TLS 配置初始化失败"; return false; }
    mbedtls_ssl_conf_authmode(&ctx->proxy_ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&ctx->proxy_ssl_config, mbedtls_ctr_drbg_random, &ctx->proxy_ctr_drbg);
    mbedtls_ssl_conf_read_timeout(&ctx->proxy_ssl_config, 10000);
    if (mbedtls_ssl_conf_alpn_protocols(&ctx->proxy_ssl_config, kAlpnProtocols) != 0 ||
        esp_crt_bundle_attach(&ctx->proxy_ssl_config) != ESP_OK ||
        mbedtls_ssl_setup(&ctx->proxy_ssl, &ctx->proxy_ssl_config) != 0 ||
        mbedtls_ssl_set_hostname(&ctx->proxy_ssl, ctx->host.c_str()) != 0) {
        ctx->last_error = "代理 TLS 初始化失败";
        return false;
    }
    mbedtls_ssl_set_bio(&ctx->proxy_ssl, &ctx->net, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);
    do { result = mbedtls_ssl_handshake(&ctx->proxy_ssl); }
    while (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (result != 0) {
        char code[16];
        snprintf(code, sizeof(code), "-0x%04x", -result);
        ctx->last_error = std::string("代理 TLS 握手失败（") + code + "）";
        return false;
    }
    ctx->proxy_tls_ready = true;
    return true;
}

bool StartTls(ProxyTransport* ctx, const char* target_host) {
    static constexpr char kPersonalization[] = "rlcd-quota-proxy";
    int result = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
                                      reinterpret_cast<const unsigned char*>(kPersonalization),
                                      sizeof(kPersonalization) - 1);
    if (result != 0) {
        ctx->last_error = "TLS 随机数初始化失败";
        return false;
    }
    result = mbedtls_ssl_config_defaults(&ctx->ssl_config, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) {
        ctx->last_error = "TLS 配置初始化失败";
        return false;
    }
    mbedtls_ssl_conf_authmode(&ctx->ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&ctx->ssl_config, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    mbedtls_ssl_conf_read_timeout(&ctx->ssl_config, 10000);
    if (mbedtls_ssl_conf_alpn_protocols(&ctx->ssl_config, kAlpnProtocols) != 0) {
        ctx->last_error = "TLS ALPN 配置失败";
        return false;
    }
    if (esp_crt_bundle_attach(&ctx->ssl_config) != ESP_OK) {
        ctx->last_error = "TLS 根证书加载失败";
        return false;
    }
    if (mbedtls_ssl_setup(&ctx->ssl, &ctx->ssl_config) != 0 ||
        mbedtls_ssl_set_hostname(&ctx->ssl, target_host) != 0) {
        ctx->last_error = "TLS SNI 初始化失败";
        return false;
    }
    mbedtls_ssl_set_bio(&ctx->ssl, ctx, ProxyTunnelSend, ProxyTunnelReceive, nullptr);
    do {
        result = mbedtls_ssl_handshake(&ctx->ssl);
    } while (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (result != 0) {
        char detail[96];
        mbedtls_strerror(result, detail, sizeof(detail));
        char code[16];
        snprintf(code, sizeof(code), "-0x%04x", -result);
        ctx->last_error = result == MBEDTLS_ERR_SSL_CONN_EOF
            ? std::string("目标在 TLS 握手时关闭连接（") + code + "）"
            : std::string("TLS 握手失败（") + code + "，" + detail + "）";
        ESP_LOGW(TAG, "%s", ctx->last_error.c_str());
        return false;
    }
    ctx->tls_ready = true;
    return true;
}

int ProxyConnect(esp_transport_handle_t transport, const char* host, int port, int timeout_ms) {
    auto* ctx = Context(transport);
    if (!ctx || !host || port <= 0) return -1;
    ctx->last_error.clear();
    const int attempts = ctx->use_tls ? kTlsRetryCount : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (rlcd::BackgroundNetworkCancelled(ctx->generation)) return -1;
        ctx->Close();
        ctx->ResetTls();
        if (!OpenProxySocket(ctx, timeout_ms)) {
            ctx->last_error = "无法连接代理端点";
            ESP_LOGW(TAG, "无法连接代理 %s:%d", ctx->host.c_str(), ctx->port);
            continue;
        }
        if (ctx->proxy_uses_tls && !StartProxyTls(ctx)) continue;
        if (!EstablishTunnel(ctx, host, port)) continue;
        if (!ctx->use_tls || StartTls(ctx, host)) return 0;
        if (attempt + 1 < attempts) ESP_LOGW(TAG, "TLS 握手失败，使用全新隧道重试一次");
    }
    ctx->Close();
    return -1;
}

int ProxyRead(esp_transport_handle_t transport, char* buffer, int length, int) {
    auto* ctx = Context(transport);
    if (!ctx || ctx->net.fd < 0) return -1;
    if (rlcd::BackgroundNetworkCancelled(ctx->generation)) return -1;
    if (!ctx->use_tls) return recv(ctx->net.fd, buffer, length, 0);
    int result = mbedtls_ssl_read(&ctx->ssl, reinterpret_cast<unsigned char*>(buffer), length);
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    return result;
}

int ProxyWrite(esp_transport_handle_t transport, const char* buffer, int length, int) {
    auto* ctx = Context(transport);
    if (!ctx || ctx->net.fd < 0) return -1;
    if (rlcd::BackgroundNetworkCancelled(ctx->generation)) return -1;
    if (!ctx->use_tls) return send(ctx->net.fd, buffer, length, 0);
    int result = mbedtls_ssl_write(&ctx->ssl,
                                   reinterpret_cast<const unsigned char*>(buffer), length);
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    return result;
}

int PollSocket(ProxyTransport* ctx, bool read, int timeout_ms) {
    if (!ctx || ctx->net.fd < 0) return -1;
    if (read && ctx->tls_ready && mbedtls_ssl_check_pending(&ctx->ssl)) return 1;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(ctx->net.fd, &set);
    timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int result = select(ctx->net.fd + 1, read ? &set : nullptr, read ? nullptr : &set,
                        nullptr, timeout_ms < 0 ? nullptr : &timeout);
    return result < 0 ? -1 : result;
}

int ProxyPollRead(esp_transport_handle_t transport, int timeout_ms) {
    return PollSocket(Context(transport), true, timeout_ms);
}

int ProxyPollWrite(esp_transport_handle_t transport, int timeout_ms) {
    return PollSocket(Context(transport), false, timeout_ms);
}

int ProxyClose(esp_transport_handle_t transport) {
    auto* ctx = Context(transport);
    if (ctx) ctx->Close();
    return 0;
}

int ProxyDestroy(esp_transport_handle_t transport) {
    delete Context(transport);
    esp_transport_set_context_data(transport, nullptr);
    return 0;
}

}  // namespace

esp_transport_handle_t CreateQuotaProxyTransport(const std::string& proxy_url,
                                                  bool use_tls,
                                                  std::string& error) {
    rlcd::ProxyUrl parsed;
    if (!rlcd::ParseProxyUrl(proxy_url, parsed, error)) {
        return nullptr;
    }
    auto* ctx = new (std::nothrow) ProxyTransport();
    if (!ctx) { error = "代理初始化内存不足"; return nullptr; }
    ctx->host = std::move(parsed.host);
    ctx->port = parsed.port;
    ctx->authorization = rlcd::ProxyAuthorizationValue(parsed);
    ctx->use_tls = use_tls;
    ctx->proxy_uses_tls = parsed.uses_tls;
    ctx->generation = rlcd::BackgroundNetworkGeneration();
    esp_transport_handle_t transport = esp_transport_init();
    if (!transport) { delete ctx; error = "代理初始化失败"; return nullptr; }
    esp_transport_set_context_data(transport, ctx);
    if (esp_transport_set_func(transport, ProxyConnect, ProxyRead, ProxyWrite, ProxyClose,
                               ProxyPollRead, ProxyPollWrite, ProxyDestroy) != ESP_OK) {
        esp_transport_destroy(transport);
        error = "代理初始化失败";
        return nullptr;
    }
    return transport;
}

std::string GetQuotaProxyTransportError(esp_transport_handle_t transport) {
    auto* ctx = Context(transport);
    return ctx ? ctx->last_error : "代理 transport 不可用";
}

std::string DiagnoseQuotaProxy(const std::string& proxy_url) {
    rlcd::BackgroundNetworkSession network_session;
    if (network_session.cancelled()) {
        return "{\"configured\":false,\"stage\":\"取消\",\"message\":\"语音交互已优先接管网络\"}";
    }
    rlcd::ProxyUrl parsed;
    std::string error;
    if (!rlcd::ParseProxyUrl(proxy_url, parsed, error)) {
        return "{\"configured\":false,\"stage\":\"配置\",\"message\":\"" + error + "\"}";
    }
    auto* transport = CreateQuotaProxyTransport(proxy_url, true, error);
    if (!transport) {
        return "{\"configured\":true,\"endpoint\":\"" + rlcd::ProxyEndpoint(parsed) +
               "\",\"stage\":\"初始化\",\"message\":\"" + error + "\"}";
    }
    const int result = esp_transport_connect(transport, "api.openai.com", 443, 12000);
    const std::string message = result == 0 ? "代理 TCP、CONNECT 与 TLS 均已通过" : GetQuotaProxyTransportError(transport);
    std::string stage = "完成";
    if (result != 0 && message.find("无法连接代理") != std::string::npos) stage = "TCP";
    else if (result != 0 && (message.find("CONNECT") != std::string::npos || message.find("认证") != std::string::npos)) stage = "CONNECT";
    else if (result != 0 && message.find("TLS") != std::string::npos) stage = "TLS";
    esp_transport_destroy(transport);
    return "{\"configured\":true,\"endpoint\":\"" + rlcd::ProxyEndpoint(parsed) +
           "\",\"tcp_connected\":" + (result == 0 ? "true" : "false") +
           ",\"stage\":\"" + stage + "\",\"message\":\"" + message + "\"}";
}
