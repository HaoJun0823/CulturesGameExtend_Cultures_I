#pragma once
#include <string>
#include <cstddef>
#include <windows.h>

// Directory layout (rooted at the main program exe directory):
//   logs/              log folder
//   plugins/           plugin folder (holds DLLs)
//   plugins/config/    plugin config
//
// All paths are resolved via GetModuleFileName(exe) instead of relying on CWD.
namespace ge_paths {

    // Directories
    constexpr const char* kLogDir     = "logs";
    constexpr const char* kPluginsDir = "plugins";
    constexpr const char* kConfigDir  = "plugins/config";

    // Config files
    constexpr const char* kGlobalIni  = "CulturesGameExtend_Global.ini";
    constexpr const char* kGameIni    = "CulturesGameExtend_Game.ini";

    // Main log file name
    constexpr const char* kMainLog    = "CulturesGameExtend.log";

    // Convenience: build a complete relative path
    inline std::string Join(const char* a, const char* b) {
        return std::string(a) + "/" + b;
    }
    inline std::string GlobalIniPath()  { return Join(kConfigDir, kGlobalIni); }
    inline std::string GameIniPath()    { return Join(kConfigDir, kGameIni); }

    // Returns the main program (exe) directory with a trailing backslash.
    inline std::string ExeDir() {
        char buf[MAX_PATH] = {};
        DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return std::string();
        std::string path(buf, n);
        size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos)
            return std::string();
        return path.substr(0, slash + 1);
    }

    // Resolve a relative path into an absolute path rooted at the exe directory.
    inline std::string Resolve(const char* rel) {
        std::string dir = ExeDir();
        if (dir.empty())
            return std::string(rel);
        return dir + rel;
    }
}
