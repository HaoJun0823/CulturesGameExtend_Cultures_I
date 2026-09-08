// dllmain.cpp : Cultures 1 GameExtend ASI plugin entry point.
//
// Loaded by Ultimate-ASI-Loader (game's built-in dinput.dll), which
// auto-loads all .asi files from the game directory.
// Initialization runs on a worker thread to avoid Loader Lock issues.
#include "pch.h"
#include "Core/Logger.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Feature.h"
#include "Core/FeatureManager.h"
#include "Core/Paths.h"

// NOTE: Features are compiled as separate translation units (see
// CulturesGameExtend.vcxproj). Do NOT #include feature .cpp files here,
// otherwise REGISTER_FEATURE runs twice (duplicate registration + stale
// COMDAT merging which silently drops the newer inline definitions).

#pragma region Log level parsing
namespace {
LogLevel ParseLogLevel(const std::string& s) {
    if (s == "Trace") return LogLevel::Trace;
    if (s == "Debug") return LogLevel::Debug;
    if (s == "Info")  return LogLevel::Info;
    if (s == "Warn")  return LogLevel::Warn;
    if (s == "Error") return LogLevel::Error;
    if (s == "None")  return LogLevel::None;
    return LogLevel::Info;
}
#pragma endregion

#pragma region Extend worker
void RunExtend(IniConfig* pcfg) {
    IniConfig& cfg = *pcfg;

    // 1) Initialize logging
    LogInit(ge_paths::kLogDir, ge_paths::kMainLog, LogLevel::Info);

    // 2) Re-apply log level from INI
    LogLevel lvl = ParseLogLevel(cfg.GetString("General", "LogLevel", "Info"));
    (void)lvl;
    LOG_INFO("[Boot]", "Log level = %s", cfg.GetString("General", "LogLevel", "Info").c_str());

    // 3) Identify game version
    GameVersion ver;
    GameTarget t = ver.Detect(cfg);
    LOG_INFO("[Boot]", "Detected game: %s (exe=%s base=0x%X)",
             GameTargetName(t), ver.ExeName().c_str(), ver.GetBaseAddress());
    if (t == GameTarget::Unknown) {
        LOG_WARN("[Boot]", "Unknown game; features still attempted where target Any.");
    }

    // 4) Install all enabled Features
    size_t n = FeatureManager::InstallAll(cfg, ver);
    LOG_INFO("[Boot]", "Extend framework initialized, %zu feature(s) active.", n);
    delete pcfg;
}
} // namespace
#pragma endregion

#pragma region DllMain entry
static DWORD WINAPI RunExtendThread(LPVOID lp) {
    IniConfig* pcfg = reinterpret_cast<IniConfig*>(lp);
    RunExtend(pcfg);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);

        // Create directories and load config (read-only, fast).
        std::error_code ec;
        ge::fs::create_directories(ge_paths::kLogDir, ec);
        ge::fs::create_directories(ge_paths::kConfigDir, ec);
        IniConfig* pcfg = new IniConfig();
        pcfg->Load(ge_paths::GlobalIniPath());
        IniConfig gameIni;
        if (gameIni.Load(ge_paths::GameIniPath())) {
            pcfg->Merge(gameIni);
        }

        // Finish init on worker thread (avoids Loader Lock).
        HANDLE hThread = CreateThread(nullptr, 0, RunExtendThread, pcfg, 0, nullptr);
        if (hThread) CloseHandle(hThread);
        else delete pcfg;
        break;
    }
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#pragma endregion
