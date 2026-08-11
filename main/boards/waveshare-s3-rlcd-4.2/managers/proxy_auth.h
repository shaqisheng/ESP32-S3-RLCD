#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace rlcd {

struct ProxyUrl {
    std::string host;
    int port = 0;
    std::string username;
    std::string password;
    bool uses_tls = false;

    bool HasAuthentication() const { return !username.empty(); }
};

inline int HexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

inline bool DecodeUrlComponent(const std::string& value, std::string& decoded) {
    decoded.clear();
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char byte = static_cast<unsigned char>(value[i]);
        if (byte == '%') {
            if (i + 2 >= value.size()) return false;
            const int high = HexDigit(value[i + 1]);
            const int low = HexDigit(value[i + 2]);
            if (high < 0 || low < 0) return false;
            byte = static_cast<unsigned char>((high << 4) | low);
            i += 2;
        }
        if (byte < 0x20 || byte == 0x7f) return false;
        decoded.push_back(static_cast<char>(byte));
    }
    return true;
}

inline bool ParseProxyUrl(const std::string& value, ProxyUrl& parsed, std::string& error) {
    constexpr const char* kHttpScheme = "http://";
    constexpr const char* kHttpsScheme = "https://";
    parsed = {};
    size_t scheme_length = 0;
    if (value.rfind(kHttpScheme, 0) == 0) {
        scheme_length = std::char_traits<char>::length(kHttpScheme);
    } else if (value.rfind(kHttpsScheme, 0) == 0) {
        scheme_length = std::char_traits<char>::length(kHttpsScheme);
        parsed.uses_tls = true;
    } else {
        error = "代理地址必须以 http:// 或 https:// 开头";
        return false;
    }
    std::string authority = value.substr(scheme_length);
    while (!authority.empty() && authority.back() == '/') authority.pop_back();
    if (authority.empty() || authority.find('/') != std::string::npos ||
        authority.find('?') != std::string::npos || authority.find('#') != std::string::npos) {
        error = "代理地址格式无效";
        return false;
    }

    const auto at = authority.rfind('@');
    std::string endpoint = authority;
    if (at != std::string::npos) {
        if (authority.find('@') != at) {
            error = "代理认证格式无效";
            return false;
        }
        const std::string userinfo = authority.substr(0, at);
        endpoint = authority.substr(at + 1);
        const auto colon = userinfo.find(':');
        if (colon == std::string::npos || colon == 0 ||
            !DecodeUrlComponent(userinfo.substr(0, colon), parsed.username) ||
            !DecodeUrlComponent(userinfo.substr(colon + 1), parsed.password) ||
            parsed.username.find(':') != std::string::npos) {
            error = "代理用户名或密码格式无效";
            return false;
        }
    }

    std::string port_text;
    if (!endpoint.empty() && endpoint.front() == '[') {
        const auto close = endpoint.find(']');
        if (close == std::string::npos || close + 2 >= endpoint.size() || endpoint[close + 1] != ':') {
            error = "代理 IPv6 地址格式无效";
            return false;
        }
        parsed.host = endpoint.substr(1, close - 1);
        port_text = endpoint.substr(close + 2);
    } else {
        const auto colon = endpoint.rfind(':');
        if (colon == std::string::npos) {
            error = "代理缺少端口";
            return false;
        }
        parsed.host = endpoint.substr(0, colon);
        port_text = endpoint.substr(colon + 1);
    }
    if (parsed.host.empty() || parsed.host.size() > 253 || port_text.empty() ||
        port_text.size() > 5 ||
        !std::all_of(port_text.begin(), port_text.end(), [](unsigned char c) { return std::isdigit(c); })) {
        error = "代理主机或端口无效";
        return false;
    }
    parsed.port = std::atoi(port_text.c_str());
    if (parsed.port <= 0 || parsed.port > 65535) {
        error = "代理端口无效";
        return false;
    }
    error.clear();
    return true;
}

inline std::string Base64Encode(const std::string& value) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((value.size() + 2) / 3) * 4);
    for (size_t i = 0; i < value.size(); i += 3) {
        const uint32_t first = static_cast<unsigned char>(value[i]);
        const uint32_t second = i + 1 < value.size() ? static_cast<unsigned char>(value[i + 1]) : 0;
        const uint32_t third = i + 2 < value.size() ? static_cast<unsigned char>(value[i + 2]) : 0;
        const uint32_t group = (first << 16) | (second << 8) | third;
        encoded.push_back(kAlphabet[(group >> 18) & 0x3f]);
        encoded.push_back(kAlphabet[(group >> 12) & 0x3f]);
        encoded.push_back(i + 1 < value.size() ? kAlphabet[(group >> 6) & 0x3f] : '=');
        encoded.push_back(i + 2 < value.size() ? kAlphabet[group & 0x3f] : '=');
    }
    return encoded;
}

inline std::string ProxyAuthorizationValue(const ProxyUrl& proxy) {
    if (!proxy.HasAuthentication()) return "";
    return "Basic " + Base64Encode(proxy.username + ":" + proxy.password);
}

inline std::string ProxyEndpoint(const ProxyUrl& proxy) {
    const bool ipv6 = proxy.host.find(':') != std::string::npos;
    return std::string(proxy.uses_tls ? "https://" : "http://") + (ipv6 ? "[" : "") + proxy.host +
           (ipv6 ? "]" : "") + ":" + std::to_string(proxy.port);
}

}  // namespace rlcd
