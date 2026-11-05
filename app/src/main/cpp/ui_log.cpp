#include "ui_log.hpp"
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {
std::mutex g_mu;
std::vector<std::string> g_lines;
constexpr size_t kMaxLines = 64;

void push_line(const char *line) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_lines.emplace_back(line);
    if (g_lines.size() > kMaxLines)
        g_lines.erase(g_lines.begin(), g_lines.begin() + (g_lines.size() - kMaxLines));
}
} // namespace

void ui_log_clear() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_lines.clear();
}

void ui_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    push_line(buf);
}

void ui_log_copy_all(char *dst, size_t cap) {
    if (!dst || !cap) return;
    dst[0] = '\0';
    std::lock_guard<std::mutex> lock(g_mu);
    size_t used = 0;
    for (size_t i = 0; i < g_lines.size(); ++i) {
        const std::string &ln = g_lines[i];
        if (used + ln.size() + 2 >= cap) break;
        if (i > 0) dst[used++] = '\n';
        std::memcpy(dst + used, ln.c_str(), ln.size());
        used += ln.size();
    }
    dst[used] = '\0';
}
