#include "pch.h"
#include "GameVersion.h"
#include "fs_compat.h"
#include "Paths.h"
#include "Logger.h"

namespace {
const char* kCategory = "[Version]";
}

const char* GameTargetName(GameTarget t) {
    switch (t) {
        case GameTarget::Cultures: return "Cultures.exe";
        case GameTarget::Any:      return "Any";
        default:                   return "Unknown";
    }
}

GameTarget GameVersion::Detect(const IniConfig& cfg) {
    m_target  = GameTarget::Unknown;
    m_exeName.clear();
    m_base    = (DWORD)(uintptr_t)GetModuleHandle(NULL);
    std::string exeName = cfg.GetString("Version", "ExeName", "Cultures.exe");

    // Version detection is based on the main program exe's directory.
    std::string exePath = ge_paths::Resolve(exeName.c_str());
    std::error_code ec;
    if (!ge::fs::exists(exePath, ec)) {
        LOG_WARN(kCategory, "Cultures.exe not found (exe dir): %s, version = Unknown", exePath.c_str());
        return m_target;
    }
    m_target  = GameTarget::Cultures;
    m_exeName = exePath;
    LOG_INFO(kCategory, "Detected game: %s (base=0x%X)", exePath.c_str(), m_base);
    return m_target;
}
