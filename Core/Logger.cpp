#include "pch.h"
#include "Logger.h"
#include "fs_compat.h"
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <fstream>

namespace {

std::mutex        g_logMutex;
std::ofstream     g_logFile;
LogLevel          g_minLevel = LogLevel::Info;
std::string       g_logPath;

const char* LevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "?????";
    }
}

std::string NowString() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

} // namespace

void LogInit(const std::string& logDir, const std::string& fileName, LogLevel minLevel) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_minLevel = minLevel;
    std::error_code ec;
    if (!logDir.empty()) {
        ge::fs::create_directories(logDir, ec);
    }
    g_logPath = logDir.empty() ? fileName : (logDir + "/" + fileName);
    g_logFile.open(g_logPath, std::ios::out | std::ios::trunc);
    if (g_logFile.is_open()) {
        g_logFile << "---- [" << NowString() << "] CulturesGameExtend log session start ----\n";
        g_logFile.flush();
    }
}

void LogWrite(const char* category, LogLevel level, const char* fmt, ...) {
    if (level < g_minLevel) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    std::string line = "[" + NowString() + "] [" + LevelName(level) + "] "
                     + (category ? category : "") + " " + buffer + "\n";
    if (g_logFile.is_open()) {
        g_logFile << line;
        g_logFile.flush();
    }
    OutputDebugStringA(line.c_str());
}
