#pragma once
#include <string>
#include "IniConfig.h"

// Target game: Cultures.exe (Cultures Gold, 1st generation).
enum class GameTarget {
    Unknown,
    Cultures,
    Any,
};

const char* GameTargetName(GameTarget t);

class GameVersion {
public:
    GameTarget Detect(const IniConfig& cfg);
    GameTarget Current() const { return m_target; }
    const std::string& ExeName() const { return m_exeName; }
    DWORD GetBaseAddress() const { return m_base; }
    bool IsCultures() const { return m_target == GameTarget::Cultures; }
private:
    GameTarget m_target = GameTarget::Unknown;
    std::string m_exeName;
    DWORD m_base = 0;
};
