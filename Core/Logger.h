#pragma once
#include <string>

// Log levels (higher value = more severe).
enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    None  = 5,
};

// Global logger initialization.
void LogInit(const std::string& logDir, const std::string& fileName, LogLevel minLevel);
void LogWrite(const char* category, LogLevel level, const char* fmt, ...);

// Convenience macros: automatically attach the category prefix.
#define LOG_TRACE(cat, ...) LogWrite(cat, LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(cat, ...) LogWrite(cat, LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(cat,  ...) LogWrite(cat, LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(cat,  ...) LogWrite(cat, LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(cat, ...) LogWrite(cat, LogLevel::Error, __VA_ARGS__)
