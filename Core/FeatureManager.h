#pragma once
#include "IniConfig.h"
#include "GameVersion.h"

class FeatureManager {
public:
    static size_t InstallAll(IniConfig& cfg, GameVersion& ver);
    static void UninstallAll();
};
