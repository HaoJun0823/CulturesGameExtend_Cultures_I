#pragma once
#include <string>
#include <unordered_map>

// Minimal INI parser (zero-dependency).
// Conventions:
//   [Section]            -> section start
//   Key = Value          -> key/value pair (leading/trailing whitespace ignored;
//                            lines starting with ';' or '#' are comments)
class IniConfig {
public:
    bool Load(const std::string& path);
    void Merge(const IniConfig& other);

    bool        IsLoaded() const { return m_loaded; }
    std::string FilePath() const { return m_path; }

    std::string GetString(const std::string& section, const std::string& key,
                          const std::string& def = "") const;
    int         GetInt(const std::string& section, const std::string& key,
                       int def = 0) const;
    bool        GetBool(const std::string& section, const std::string& key,
                        bool def = false) const;
    bool HasKey(const std::string& section, const std::string& key) const;

private:
    static std::string Normalize(const std::string& s);
    bool m_loaded = false;
    std::string m_path;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> m_data;
};
