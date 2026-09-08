#pragma once
#include <string>
#include <vector>
#include "IniConfig.h"
#include "GameVersion.h"

class FeatureManager;
class Feature {
public:
    virtual ~Feature() = default;
    virtual const char* GetName() const = 0;
    virtual GameTarget GetTarget() const { return GameTarget::Any; }
    virtual bool OnInstall(IniConfig& cfg, GameVersion& ver) = 0;
    bool TargetMatch(GameVersion& ver) const;
};

class FeatureRegistry {
public:
    static FeatureRegistry& Instance();
    void Register(Feature* f);
    const std::vector<Feature*>& All() const { return m_features; }
private:
    FeatureRegistry() = default;
    std::vector<Feature*> m_features;
};

// Self-registration macro.
#define REGISTER_FEATURE(Cls) \
    namespace { \
        struct Cls##_Registrar { \
            Cls##_Registrar() { FeatureRegistry::Instance().Register(new Cls()); } \
        } Cls##_registrar_inst; \
    }
