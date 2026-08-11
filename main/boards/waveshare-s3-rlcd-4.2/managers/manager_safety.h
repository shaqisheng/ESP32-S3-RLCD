#pragma once

#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace rlcd {

// RLCD 内部 SRAM 很紧张，多个 TLS/HTTP 请求并发时会挤占 MQTT 和屏幕 SPI
// 所需的 DMA 内存。所有低优先级后台联网任务共用这把锁，保证逐个执行。
inline std::mutex& BackgroundNetworkMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::atomic<uint32_t>& BackgroundNetworkGenerationStorage() {
    static std::atomic<uint32_t> generation{1};
    return generation;
}

inline std::atomic<bool>& BackgroundNetworkActiveStorage() {
    static std::atomic<bool> active{false};
    return active;
}

inline uint32_t BackgroundNetworkGeneration() {
    return BackgroundNetworkGenerationStorage().load();
}

inline void CancelBackgroundNetwork() {
    BackgroundNetworkGenerationStorage().fetch_add(1);
}

inline bool BackgroundNetworkCancelled(uint32_t generation) {
    return BackgroundNetworkGeneration() != generation;
}

inline bool IsBackgroundNetworkActive() {
    return BackgroundNetworkActiveStorage().load();
}

inline std::atomic<bool>& BootDataPrefetchCompletedStorage() {
    static std::atomic<bool> completed{false};
    return completed;
}

inline std::atomic<bool>& BootTimePrefetchedStorage() {
    static std::atomic<bool> succeeded{false};
    return succeeded;
}

inline std::atomic<bool>& BootWeatherPrefetchedStorage() {
    static std::atomic<bool> succeeded{false};
    return succeeded;
}

inline void MarkBootDataPrefetched(bool time_succeeded, bool weather_succeeded) {
    BootTimePrefetchedStorage().store(time_succeeded);
    BootWeatherPrefetchedStorage().store(weather_succeeded);
    BootDataPrefetchCompletedStorage().store(true);
}

inline bool BootDataPrefetchCompleted() {
    return BootDataPrefetchCompletedStorage().load();
}

inline bool BootTimePrefetched() {
    return BootTimePrefetchedStorage().load();
}

inline bool BootWeatherPrefetched() {
    return BootWeatherPrefetchedStorage().load();
}

class BackgroundNetworkSession {
public:
    explicit BackgroundNetworkSession(uint32_t generation = BackgroundNetworkGeneration())
        : lock_(BackgroundNetworkMutex()), generation_(generation) {
        BackgroundNetworkActiveStorage().store(true);
    }

    ~BackgroundNetworkSession() {
        BackgroundNetworkActiveStorage().store(false);
    }

    uint32_t generation() const { return generation_; }
    bool cancelled() const { return BackgroundNetworkCancelled(generation_); }

private:
    std::unique_lock<std::mutex> lock_;
    uint32_t generation_;
};

inline bool IsStrictIsoDate(const std::string& value) {
    if (value.empty()) return true;
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
    }

    const auto parse_digits = [&](size_t begin, size_t count) {
        int result = 0;
        for (size_t i = begin; i < begin + count; ++i) {
            result = result * 10 + (value[i] - '0');
        }
        return result;
    };
    const int year = parse_digits(0, 4);
    const int month = parse_digits(5, 2);
    const int day = parse_digits(8, 2);
    if (month < 1 || month > 12 || day < 1) return false;
    static constexpr int kDaysInMonth[] = {31, 28, 31, 30, 31, 30,
                                           31, 31, 30, 31, 30, 31};
    int max_day = kDaysInMonth[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) max_day = 29;
    return day <= max_day;
}

enum class StatusVisibility {
    kPublicOnly,
    kIncludePrivateQuota,
};

inline StatusVisibility StatusVisibilityFor(bool authenticated) {
    return authenticated ? StatusVisibility::kIncludePrivateQuota
                         : StatusVisibility::kPublicOnly;
}

inline std::string FormatAdminAddress(const std::string& ip_address) {
    if (ip_address.empty() || ip_address == "0.0.0.0") {
        return "管理后台 · 等待网络";
    }
    return "管理 · http://" + ip_address + ":8080/admin";
}

template <typename T>
class ThreadSafeSnapshot {
public:
    T Get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

    void Set(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = std::move(value);
    }

    template <typename Mutator>
    void Modify(Mutator&& mutator) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::forward<Mutator>(mutator)(value_);
    }

private:
    mutable std::mutex mutex_;
    T value_{};
};

template <typename T, typename Mutator, typename Validator, typename Persister>
bool CommitValidatedUpdate(T& current,
                           Mutator&& mutate,
                           Validator&& validate,
                           Persister&& persist) {
    T candidate = current;
    std::forward<Mutator>(mutate)(candidate);
    if (!std::forward<Validator>(validate)(candidate)) return false;
    if (!std::forward<Persister>(persist)(candidate)) return false;
    current = std::move(candidate);
    return true;
}

}  // namespace rlcd
