#include "pch.h"
#include "FeatureManager.h"
#include "Feature.h"
#include "Logger.h"

namespace {
const char* kCategory = "[FeatureManager]";
}

bool Feature::TargetMatch(GameVersion& ver) const {
    GameTarget t = GetTarget();
    if (t == GameTarget::Any) return true;
    return (t == ver.Current());
}

FeatureRegistry& FeatureRegistry::Instance() {
    static FeatureRegistry inst;
    return inst;
}

void FeatureRegistry::Register(Feature* f) {
    if (f) m_features.push_back(f);
}

size_t FeatureManager::InstallAll(IniConfig& cfg, GameVersion& ver) {
    size_t ok = 0;
    size_t attempted = 0;
    for (Feature* f : FeatureRegistry::Instance().All()) {
        const char* name = f->GetName();
        bool enabled = cfg.GetBool(name, "Enabled", false);
        if (!enabled) {
            LOG_INFO(kCategory, "Feature '%s' disabled (Enabled=0), skip.", name);
            continue;
        }
        if (!f->TargetMatch(ver)) {
            LOG_WARN(kCategory, "Feature '%s' skipped: target mismatch.", name);
            continue;
        }
        ++attempted;
        LOG_INFO(kCategory, "Installing Feature '%s' ...", name);
        if (f->OnInstall(cfg, ver)) {
            ++ok;
            LOG_INFO(kCategory, "Feature '%s' installed OK.", name);
        } else {
            LOG_ERROR(kCategory, "Feature '%s' install FAILED.", name);
        }
    }
    LOG_INFO(kCategory, "Installed %zu/%zu features (%zu registered).",
             ok, attempted, FeatureRegistry::Instance().All().size());
    return ok;
}

void FeatureManager::UninstallAll() {
}
