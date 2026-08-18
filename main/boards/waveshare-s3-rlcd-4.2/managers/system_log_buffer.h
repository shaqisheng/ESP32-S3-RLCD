#pragma once

// 系统日志环形缓冲：钩住 esp_log 全局输出，供后台 /api/logs 读取。
//
// 设计要点：
// - 缓冲驻留 PSRAM（约 45KB），不占用紧张的内部 SRAM（见 ARCHITECTURE §6.2）。
// - vprintf 钩子在所有任务的日志上下文运行：只做本地格式化 + 自旋锁内 memcpy，
//   不 malloc、不阻塞；UART 原样输出不受影响。
// - 连续重复行（如 AFE 每 30ms 的 ringbuffer 告警）折叠为 repeat 计数，避免冲掉有用日志。
// - key=/token=/password=/secret= 的参数值打码后才入库，防止 secret 经日志接口泄露。

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <strings.h>

class SystemLogBuffer {
public:
    struct Entry {
        uint32_t seq;       // 单调递增序号（含被覆盖的），客户端增量拉取用
        uint32_t log_ms;    // esp_log_timestamp()，启动毫秒
        uint16_t repeat;    // 连续重复次数（1 = 未重复）
        char level;         // 'I'/'W'/'E'/'D'/'V'，解析失败为 '?'
        char tag[20];
        char text[140];
    };
    static constexpr int kCapacity = 256;

    static SystemLogBuffer& GetInstance() {
        static SystemLogBuffer instance;
        return instance;
    }

    // 安装 vprintf 钩子。在板级初始化尽早调用（非网络服务，构造函数可调用）。
    void Install() {
        if (installed_) return;
        entries_ = static_cast<Entry*>(heap_caps_calloc(kCapacity, sizeof(Entry), MALLOC_CAP_SPIRAM));
        if (!entries_) {
            ESP_LOGE("SystemLog", "日志缓冲 PSRAM 分配失败（%u B），日志功能不可用",
                     (unsigned)(kCapacity * sizeof(Entry)));
            return;
        }
        esp_log_set_vprintf(&SystemLogBuffer::VprintfHook);
        installed_ = true;
    }

    // 序列化为 JSON：返回 seq > after_seq 的最多 limit 条（正序）。
    // 形如 {"entries":[{"seq":1,"t":1786956000,"lv":"I","tag":"WiFi","text":"...","r":1}...],
    //       "latest_seq":N,"dropped":N,"capacity":256}
    std::string ToJson(uint32_t after_seq, int limit) {
        if (limit <= 0) limit = 50;
        if (limit > 100) limit = 100;
        std::string out;
        out.reserve(4096);
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const time_t now_epoch = time(nullptr);
        out += "{\"entries\":[";
        bool first = true;
        if (entries_) {
            // 自旋锁内只做一次 Entry 拷贝（172B），snprintf/malloc 都在锁外——
            // 持锁格式化会触发 heap 断言（malloc 不允许在临界区）。
            const uint32_t total = seq_counter_;
            const uint32_t start_seq = total > kCapacity ? total - kCapacity : 0;
            uint32_t from = after_seq > start_seq ? after_seq + 1 : start_seq + 1;
            if (from + limit - 1 > total && total >= (uint32_t)limit) from = total - limit + 1;
            for (uint32_t seq = from; seq <= total; ++seq) {
                Entry e;
                portENTER_CRITICAL(&mux_);
                e = entries_[(seq - 1) % kCapacity];
                portEXIT_CRITICAL(&mux_);
                if (e.seq != seq) continue;  // 拷贝间隙被覆盖则跳过，防御性
                if (!first) out += ',';
                first = false;
                char head[96];
                // epoch 秒在 2106 年前远小于 2^32，用 %u 打印——NEWLIB_NANO_FORMAT
                // 不支持 %lld（会原样输出 "ld" 并错位后续 varargs）。
                const uint32_t epoch = (uint32_t)((int64_t)now_epoch - (now_ms - e.log_ms) / 1000);
                snprintf(head, sizeof(head), "{\"seq\":%u,\"t\":%u,\"lv\":\"%c\",\"tag\":\"",
                         (unsigned)e.seq, (unsigned)epoch, e.level);
                out += head;
                AppendEscaped(out, e.tag);
                out += "\",\"text\":\"";
                AppendEscaped(out, e.text);
                snprintf(head, sizeof(head), "\",\"r\":%u}", (unsigned)e.repeat);
                out += head;
            }
        }
        char tail[96];
        snprintf(tail, sizeof(tail), "],\"latest_seq\":%u,\"dropped\":%u,\"capacity\":%d}",
                 (unsigned)seq_counter_, (unsigned)dropped_, kCapacity);
        out += tail;
        return out;
    }

private:
    SystemLogBuffer() { mux_ = portMUX_INITIALIZER_UNLOCKED; }

    static int VprintfHook(const char* fmt, va_list args) {
        // UART 原样输出（va_list 只能消费一次，先复制）
        va_list copy;
        va_copy(copy, args);
        vprintf(fmt, args);
        char line[192];
        const int n = vsnprintf(line, sizeof(line), fmt, copy);
        va_end(copy);
        if (n > 0) GetInstance().Push(line, n >= (int)sizeof(line) ? (int)sizeof(line) - 1 : n);
        return n;
    }

    void Push(const char* line, int len) {
        // 剥掉尾部换行/回车
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) --len;
        if (len <= 0) return;

        // 解析默认格式 "I (12345) TAG: 文本"；解析失败则整行作为文本
        char level = '?';
        char tag[20] = "";
        const char* text = line;
        if (len > 3 && strchr("EWIDV", line[0]) && line[1] == ' ' && line[2] == '(') {
            const char* close = static_cast<const char*>(memchr(line, ')', len));
            if (close && close + 2 < line + len && close[1] == ' ') {
                const char* tag_begin = close + 2;
                const char* colon = static_cast<const char*>(memchr(tag_begin, ':', line + len - tag_begin));
                if (colon && colon > tag_begin) {
                    level = line[0];
                    const int tag_len = (colon - tag_begin) < (int)sizeof(tag) - 1
                        ? (int)(colon - tag_begin) : (int)sizeof(tag) - 1;
                    memcpy(tag, tag_begin, tag_len);
                    tag[tag_len] = '\0';
                    text = colon + 1;
                    if (text < line + len && *text == ' ') ++text;
                }
            }
        }

        // 打码敏感参数：key=/token=/password=/secret= 的值（到 &、空格或行尾）
        char masked[sizeof(((Entry*)nullptr)->text)];
        MaskSecrets(text, masked, sizeof(masked));

        portENTER_CRITICAL(&mux_);
        if (!entries_) { portEXIT_CRITICAL(&mux_); return; }
        // 连续重复折叠：与上一条 level/tag/text 全同则只累加计数
        if (seq_counter_ > 0) {
            Entry& last = entries_[(seq_counter_ - 1) % kCapacity];
            if (last.seq == seq_counter_ && last.level == level && strcmp(last.tag, tag) == 0 &&
                strcmp(last.text, masked) == 0) {
                if (last.repeat < 0xFFFF) ++last.repeat;
                portEXIT_CRITICAL(&mux_);
                return;
            }
        }
        Entry& e = entries_[seq_counter_ % kCapacity];
        if (seq_counter_ >= kCapacity) ++dropped_;
        ++seq_counter_;
        e.seq = seq_counter_;
        e.log_ms = esp_log_timestamp();
        e.repeat = 1;
        e.level = level;
        strncpy(e.tag, tag, sizeof(e.tag) - 1);
        e.tag[sizeof(e.tag) - 1] = '\0';
        memcpy(e.text, masked, sizeof(e.text));
        portEXIT_CRITICAL(&mux_);
    }

    static void MaskSecrets(const char* in, char* out, size_t out_size) {
        static const char* kKeys[] = {"key=", "token=", "password=", "secret="};
        size_t o = 0;
        const size_t n = strlen(in);
        for (size_t i = 0; i < n && o + 1 < out_size;) {
            bool masked_key = false;
            for (const char* k : kKeys) {
                const size_t kl = strlen(k);
                if (i + kl <= n && strncasecmp(in + i, k, kl) == 0) {
                    memcpy(out + o, in + i, kl);
                    o += kl;
                    i += kl;
                    const char* mask = "****";
                    memcpy(out + o, mask, 4);
                    o += 4;
                    while (i < n && in[i] != '&' && in[i] != ' ') ++i;
                    masked_key = true;
                    break;
                }
            }
            if (!masked_key) out[o++] = in[i++];
        }
        out[o] = '\0';
    }

    static void AppendEscaped(std::string& out, const char* s) {
        for (const char* p = s; *p; ++p) {
            const unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                out += '\\';
                out += (char)c;
            } else if (c >= 0x20) {
                out += (char)c;  // 控制字符直接丢弃（日志文本无需换行符）
            }
        }
    }

    Entry* entries_ = nullptr;
    portMUX_TYPE mux_;
    uint32_t seq_counter_ = 0;
    uint32_t dropped_ = 0;
    bool installed_ = false;
};
