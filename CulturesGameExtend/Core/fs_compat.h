#pragma once
// Filesystem compatibility layer.
// v141 / v141_xp toolset (VS2017, _MSC_VER < 1920) has no official <filesystem>,
// uses <experimental/filesystem>; v142+ (VS2019+, _MSC_VER >= 1920) uses <filesystem>.
// Exposed uniformly under the ge::fs namespace.

#if defined(_MSC_VER) && (_MSC_VER < 1920)
    #include <experimental/filesystem>
    namespace ge { namespace fs = std::experimental::filesystem; }
#else
    #include <filesystem>
    namespace ge { namespace fs = std::filesystem; }
#endif

#include <string>

namespace ge {

// Tolerantly strip UTF-8 BOM (EF BB BF) so text data files with or without a
// BOM parse correctly.
inline void StripUtf8Bom(std::string& s) {
    if (s.size() >= 3 &&
        (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) {
        s.erase(0, 3);
    }
}

} // namespace ge
