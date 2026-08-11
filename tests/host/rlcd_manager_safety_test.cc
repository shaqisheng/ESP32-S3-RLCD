#include <atomic>
#include <cassert>
#include <string>
#include <thread>
#include <vector>

#include "manager_safety.h"
#include "proxy_auth.h"

namespace {

struct SnapshotValue {
    std::string city;
    std::string text;
    int generation = 0;
};

struct TodoValue {
    std::string content;
    std::string due_date;
};

void TestStrictIsoDate() {
    assert(rlcd::IsStrictIsoDate(""));
    assert(rlcd::IsStrictIsoDate("2024-02-29"));
    assert(!rlcd::IsStrictIsoDate("2023-02-29"));
    assert(!rlcd::IsStrictIsoDate("2026-13-01"));
    assert(!rlcd::IsStrictIsoDate("2026-04-31"));
    assert(!rlcd::IsStrictIsoDate("abcd-ef-gh"));
    assert(!rlcd::IsStrictIsoDate("2026-8-10"));
}

void TestStatusVisibility() {
    assert(rlcd::StatusVisibilityFor(false) == rlcd::StatusVisibility::kPublicOnly);
    assert(rlcd::StatusVisibilityFor(true) == rlcd::StatusVisibility::kIncludePrivateQuota);
}

void TestBootDataPrefetchStatePublishesResultsAtomically() {
    assert(!rlcd::BootDataPrefetchCompleted());
    rlcd::MarkBootDataPrefetched(true, false);
    assert(rlcd::BootDataPrefetchCompleted());
    assert(rlcd::BootTimePrefetched());
    assert(!rlcd::BootWeatherPrefetched());
}

void TestAdminAddressFormatting() {
    assert(rlcd::FormatAdminAddress("192.168.2.71") ==
           "管理 · http://192.168.2.71:8080/admin");
    assert(rlcd::FormatAdminAddress("") == "管理后台 · 等待网络");
    assert(rlcd::FormatAdminAddress("0.0.0.0") == "管理后台 · 等待网络");
}

void TestAuthenticatedProxyUrl() {
    rlcd::ProxyUrl parsed;
    std::string error;
    assert(rlcd::ParseProxyUrl("http://demo%2Duser:p%40ss@192.0.2.8:7892", parsed, error));
    assert(parsed.host == "192.0.2.8");
    assert(parsed.port == 7892);
    assert(parsed.username == "demo-user");
    assert(parsed.password == "p@ss");
    assert(rlcd::ProxyAuthorizationValue(parsed) == "Basic ZGVtby11c2VyOnBAc3M=");
    assert(rlcd::ProxyEndpoint(parsed) == "http://192.0.2.8:7892");
}

void TestHttpsAuthenticatedProxyUrl() {
    rlcd::ProxyUrl parsed;
    std::string error;
    assert(rlcd::ParseProxyUrl("https://demo-user:demo-password@proxy.example.test:7893", parsed, error));
    assert(parsed.host == "proxy.example.test");
    assert(parsed.port == 7893);
    assert(parsed.uses_tls);
    assert(rlcd::ProxyEndpoint(parsed) == "https://proxy.example.test:7893");
}

void TestProxyUrlRejectsCredentialInjection() {
    rlcd::ProxyUrl parsed;
    std::string error;
    assert(!rlcd::ParseProxyUrl("http://demo:bad%0D%0AHeader@192.0.2.8:7892", parsed, error));
}

void TestValidatedUpdateIsAtomic() {
    TodoValue current{"原待办", "2026-08-10"};
    bool persisted = false;
    const bool invalid = rlcd::CommitValidatedUpdate(
        current,
        [](TodoValue& candidate) { candidate.due_date = "2026-02-30"; },
        [](const TodoValue& candidate) { return rlcd::IsStrictIsoDate(candidate.due_date); },
        [&](const TodoValue&) { persisted = true; return true; });
    assert(!invalid);
    assert(!persisted);
    assert(current.due_date == "2026-08-10");

    const bool save_failed = rlcd::CommitValidatedUpdate(
        current,
        [](TodoValue& candidate) { candidate.content = "新待办"; },
        [](const TodoValue&) { return true; },
        [](const TodoValue&) { return false; });
    assert(!save_failed);
    assert(current.content == "原待办");

    const bool updated = rlcd::CommitValidatedUpdate(
        current,
        [](TodoValue& candidate) { candidate.content = "新待办"; },
        [](const TodoValue&) { return true; },
        [](const TodoValue& candidate) { return candidate.content == "新待办"; });
    assert(updated);
    assert(current.content == "新待办");
}

void TestSnapshotReadsAreCoherent() {
    rlcd::ThreadSafeSnapshot<SnapshotValue> snapshot;
    snapshot.Set({"city0", "text0", 0});
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!start.load()) {}
            for (int n = 0; n < 20000; ++n) {
                const auto value = snapshot.Get();
                if (value.city != "city" + std::to_string(value.generation) ||
                    value.text != "text" + std::to_string(value.generation)) {
                    failed.store(true);
                    return;
                }
            }
        });
    }
    start.store(true);
    for (int generation = 1; generation <= 20000; ++generation) {
        snapshot.Set({"city" + std::to_string(generation),
                      "text" + std::to_string(generation), generation});
    }
    for (auto& reader : readers) reader.join();
    assert(!failed.load());
}

}  // namespace

int main() {
    TestStrictIsoDate();
    TestStatusVisibility();
    TestBootDataPrefetchStatePublishesResultsAtomically();
    TestAdminAddressFormatting();
    TestAuthenticatedProxyUrl();
    TestHttpsAuthenticatedProxyUrl();
    TestProxyUrlRejectsCredentialInjection();
    TestValidatedUpdateIsAtomic();
    TestSnapshotReadsAreCoherent();
    return 0;
}
