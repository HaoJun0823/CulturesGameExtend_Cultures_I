// TextReplacementFeature.cpp
// ===================================================================
// Cultures 1 (Gold) 文本替换 hook
//
// Hook 目标: sub_48DEA0 @ 0x48DEA0 (universal text lookup)
//   __thiscall(ecx=text_lib_obj, [esp+4]=index), ret 4
//   返回: const char* (Latin-1/CP1252 德语文本指针, 或 nullptr)
//
//   原始入口 (7 bytes = 2 complete instructions):
//     8B 44 24 04    mov eax, [esp+arg_0]   ; index
//     8B 51 04       mov edx, [ecx+4]       ; count
//   → 5-byte JMP + 2 NOP, trampoline 复制 7 字节
//
//   策略 (post-call):
//     1. Stub 调用 C handler OnTextLookup(this, index)
//     2. C handler 调 trampoline (原始函数) → 得到德语 char*
//     3. 用德语文本内容查字典 (key=CP1252 German, value=UTF-8 Chinese)
//     4. 命中 → 返回中文 UTF-8 指针 (注册到 g_chineseStrs)
//     5. 未命中 → 返回原始德语指针
//
//   此 hook 覆盖所有文本查询路径:
//     - .sal 路径: sub_473BE2 → sub_473998 → sub_48DEA0
//     - .tab 路径: sub_474E92 → sub_48DEA0
//     - 其他 49 个调用者全部覆盖
//
// 字典格式 (translation.tsv):
//   german_text<TAB>chinese_utf8
//   以 # 开头的行为注释
//   文件编码: UTF-8 (German 列加载时转为 CP1252 作为 key)
// ===================================================================

#include "pch.h"
#include "Core/Feature.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Paths.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <sstream>

// ---- TextRendererFeature 的 g_chineseStrs 外部声明 ----
extern std::unordered_set<const char*> g_chineseStrs;

namespace {

const char* kCat = "[TextReplace]";

// ---- Game addresses ----
constexpr uintptr_t ADDR_TEXT_LOOKUP = 0x48DEA0;  // sub_48DEA0

// ---- Trampoline (原始函数入口副本) ----
// 声明为 __thiscall 函数指针，可直接用 (this, index) 调用
typedef const char* (__thiscall *OrigTextLookupFn)(void* this_ptr, uint32_t index);
OrigTextLookupFn g_origTextLookup = nullptr;

// ---- sub_48DED0: 字符串表写入器（strlen 空指针崩） ----
// __thiscall(ecx=table, [esp+4]=index, [esp+8]=str), ret 8
// 游戏在解析 GUI/text 文件时，无引号/空值条目会让 sub_482C30 返回 NULL 字符串，
// 而 sub_48DED0 内部直接 strlen(str) 不判空 → 0x48DF4D 处对地址 0 读取 → AV 崩溃。
// 钩子在入口把 NULL 替换成空串，再 relay 到原始函数，彻底消除该崩溃。
constexpr uintptr_t ADDR_TABLE_WRITER = 0x48DED0;
typedef int (__thiscall *OrigTableWriterFn)(void* table, uint32_t index, const char* str);
OrigTableWriterFn g_origTableWriter = nullptr;
static char kEmptyTableString[] = { 0 };

// ---- Translation dictionary ----
// Key: German text (CP1252 bytes, std::string)
// Value: Chinese text (UTF-8, std::string, 持久存储)
std::unordered_map<std::string, std::string> g_dict;

// ---- 统计 ----
int g_callCount = 0;
int g_hitCount = 0;
int g_missCount = 0;
bool g_firstCall = true;

// ---- 未命中去重日志 ----
std::unordered_set<std::string> g_loggedMisses;
int g_maxMissLogs = 64;

// ===================================================================
// Utf8ToCp1252: 将 UTF-8 字符串转为 CP1252 (Windows-1252) 字节
// 用于把 TSV 中的 German 列从 UTF-8 转为游戏实际编码
// ===================================================================
std::string Utf8ToCp1252(const std::string& utf8) {
    if (utf8.empty()) return std::string();
    // UTF-8 → UTF-16
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (wlen <= 0) return utf8;  // fallback
    std::wstring wide(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wide[0], wlen);
    // UTF-16 → CP1252
    int blen = WideCharToMultiByte(1252, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    if (blen <= 0) return utf8;  // fallback
    std::string cp1252(blen, 0);
    WideCharToMultiByte(1252, 0, wide.c_str(), (int)wide.size(), &cp1252[0], blen, nullptr, nullptr);
    return cp1252;
}

// ===================================================================
// LoadTranslation: 从 TSV 文件加载翻译字典
// 格式: german\tchinese (UTF-8 file)
// ===================================================================
bool LoadTranslation(const char* path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_ERROR(kCat, "Cannot open translation file: %s", path);
        return false;
    }

    std::string line;
    int lineNum = 0;
    int loaded = 0;

    while (std::getline(f, line)) {
        lineNum++;
        if (line.empty() || line[0] == '#') continue;

        // 解析 TSV: german \t chinese
        // 注意: getline 保留 \r（如果文件有 CRLF），需要去除
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;

        std::string german  = line.substr(0, tab);
        std::string chinese = line.substr(tab + 1);
        if (german.empty() || chinese.empty()) continue;

        // German 列: UTF-8 → CP1252 (匹配游戏内存中的实际编码)
        std::string key = Utf8ToCp1252(german);

        g_dict[key] = chinese;
        loaded++;
    }

    LOG_INFO(kCat, "Loaded %d translations from %s (%d lines)",
             loaded, path, lineNum);
    return loaded > 0;
}

// ===================================================================
// OnTextLookup: C handler for sub_48DEA0 hook (post-call)
//   this_ptr = text_lib_obj (ecx 原始值)
//   index    = string index
//   Returns: const char* (中文 UTF-8 或原始德语)
//
// 策略 (in-place 覆写):
//   调用原始函数得到德语 char* (指向游戏内部缓冲区)
//   查字典命中 → 将中文 UTF-8 直接覆写到游戏缓冲区
//   只要中文长度(字节) <= 原文长度(字节)即可安全覆写
//   返回原始指针(游戏内部缓冲区)，避免堆指针导致游戏崩溃
// ===================================================================
const char* OnTextLookup(void* this_ptr, uint32_t index) {
    g_callCount++;

    // 调用原始函数获取德语文本
    const char* orig = g_origTextLookup(this_ptr, index);

    if (!orig || !*orig) return orig;

    if (g_dict.empty()) return orig;

    // 查字典，命中时就地覆写游戏内部缓冲区
    auto it = g_dict.find(orig);
    if (it != g_dict.end()) {
        g_hitCount++;
        const std::string& cn = it->second;
        size_t cn_len = cn.size();
        size_t orig_len = strlen(orig);

        if (cn_len <= orig_len) {
            // 中文(含NUL)可以安全放入原文的空间
            memcpy((void*)orig, cn.c_str(), cn_len + 1);
            if (g_hitCount <= 32) {
                LOG_INFO(kCat, "HIT [%d] in-place %zu/%zu bytes", g_hitCount, cn_len, orig_len);
            }
        } else {
            // 中文比原文长——跳过避免缓冲区溢出
            if (g_hitCount <= 32) {
                LOG_INFO(kCat, "SKIP [%d] too long %zu/%zu bytes", g_hitCount, cn_len, orig_len);
            }
        }
        // 无论覆写与否，都返回原始指针(游戏内部缓冲区)
        return orig;
    }

    return orig;
}

// ===================================================================
// MakeTrampoline: copy original bytes + E9 jmp back
// ===================================================================
void* MakeTrampoline(uintptr_t entry, int copyLen) {
    void* t = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    auto orig = Patch::ReadBytes(entry, copyLen);
    if (orig.size() != (size_t)copyLen) {
        VirtualFree(t, 0, MEM_RELEASE);
        return nullptr;
    }
    memcpy(t, orig.data(), copyLen);
    uintptr_t back = entry + copyLen;
    *(uint8_t*)((uintptr_t)t + copyLen) = 0xE9;
    *(int32_t*)((uintptr_t)t + copyLen + 1) =
        (int32_t)(back - ((uintptr_t)t + copyLen + 5));
    return t;
}

// ===================================================================
// Naked stub for sub_48DEA0 (__thiscall, ret 4)
//
// 入口: ecx=this (text_lib_obj), [esp+4]=index
//
// stub 逻辑:
//   1. 保存 ecx 到 ebx (非易失寄存器)
//   2. 调用 OnTextLookup(this, index) — cdecl
//   3. eax = 返回的 char* (中文或德语)
//   4. pop ebx, ret 4 (callee cleans 4 bytes)
//
// 注意: 无 relay 路径 — OnTextLookup 内部调用 trampoline 获取原始结果
// ===================================================================
extern "C" void __declspec(naked) GetTextStub() {
    __asm {
        // 入口: ecx=this, [esp+4]=index
        push ebx
        mov ebx, ecx               // save this
        push dword ptr [esp+8]     // index (push ebx 后偏移 +4: [esp+8]=原始 [esp+4])
        push ebx                    // this
        call OnTextLookup
        add esp, 8                  // cleanup cdecl args
        pop ebx
        ret 4                       // callee cleans index arg
    }
}

// sub_48DED0 空字符串防护。
// 入口: ecx=table, [esp+4]=index, [esp+8]=str (ret 8)。
// 仅当 str==NULL 时原地替换为空串指针，然后原样 relay 到 trampoline。
extern "C" void __declspec(naked) TableWriterGuardStub() {
    __asm {
        push ebx
        mov ebx, ecx               // 暂存 this（非易失）
        mov eax, dword ptr [esp+0x0C]  // str（push ebx 前是 [esp+8]）
        test eax, eax
        jnz twg_ok
        mov eax, offset kEmptyTableString
        mov dword ptr [esp+0x0C], eax  // NULL -> 空串
    twg_ok:
        mov ecx, ebx               // 恢复 this
        pop ebx
        jmp dword ptr [g_origTableWriter]  // relay 到原始函数
    }
}

} // namespace

// ===================================================================
// Feature class
// ===================================================================
class TextReplacementFeature : public Feature {
public:
    const char* GetName() const override { return "TextReplace"; }
    GameTarget GetTarget() const override { return GameTarget::Cultures; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        // ---- Load translation file ----
        std::string tsvPath = ge_paths::Resolve("plugins/translation.tsv");
        if (!LoadTranslation(tsvPath.c_str())) {
            LOG_WARN(kCat, "No translation loaded; hook will pass-through");
        }

        // ---- Verify hook point bytes ----
        auto bytes = Patch::ReadBytes(ADDR_TEXT_LOOKUP, 12);
        // Expected: 8B 44 24 04 8B 51 04 (mov eax,[esp+4]; mov edx,[ecx+4])
        if (bytes.size() >= 7 &&
            bytes[0] == 0x8B && bytes[1] == 0x44 && bytes[2] == 0x24 &&
            bytes[3] == 0x04 && bytes[4] == 0x8B && bytes[5] == 0x51 &&
            bytes[6] == 0x04) {
            LOG_INFO(kCat, "text_lookup bytes OK: 8B 44 24 04 8B 51 04");
        } else {
            LOG_ERROR(kCat, "text_lookup bytes MISMATCH!");
            LOG_ERROR(kCat, "  Got: %02X %02X %02X %02X %02X %02X %02X",
                     bytes[0], bytes[1], bytes[2], bytes[3],
                     bytes[4], bytes[5], bytes[6]);
            return false;
        }

        // ---- Create trampoline (7 bytes = 2 complete instructions) ----
        void* tramp = MakeTrampoline(ADDR_TEXT_LOOKUP, 7);
        if (!tramp) {
            LOG_ERROR(kCat, "Failed to create text_lookup trampoline");
            return false;
        }
        g_origTextLookup = (OrigTextLookupFn)tramp;

        // ---- Write JMP (5 bytes) + 2 NOP (cover 7-byte window) ----
        if (!Patch::WriteJmp(ADDR_TEXT_LOOKUP, (uintptr_t)&GetTextStub, 2)) {
            LOG_ERROR(kCat, "Failed to write text_lookup JMP");
            return false;
        }

        LOG_INFO(kCat, "Hooked text_lookup @ 0x%X -> stub=%p tramp=%p origFn=%p",
                 (unsigned)ADDR_TEXT_LOOKUP,
                 (void*)&GetTextStub, tramp, (void*)g_origTextLookup);

        // ---- Hook sub_48DED0: 空字符串防护（修 GUI/text 加载崩溃 0x48DF4D） ----
        // Entry 8 字节: 53 8B D9 57 8B 7C 24 0C
        //   (push ebx; mov ebx,ecx; push edi; mov edi,[esp+0xC])
        auto twBytes = Patch::ReadBytes(ADDR_TABLE_WRITER, 12);
        if (twBytes.size() >= 8 &&
            twBytes[0] == 0x53 && twBytes[1] == 0x8B && twBytes[2] == 0xD9 && twBytes[3] == 0x57 &&
            twBytes[4] == 0x8B && twBytes[5] == 0x7C && twBytes[6] == 0x24 && twBytes[7] == 0x0C) {
            void* twTramp = MakeTrampoline(ADDR_TABLE_WRITER, 8);
            if (!twTramp) {
                LOG_ERROR(kCat, "Failed to create table_writer trampoline");
                return false;
            }
            g_origTableWriter = (OrigTableWriterFn)twTramp;
            // 5-byte JMP + 3 NOP 覆盖 8 字节窗口
            if (!Patch::WriteJmp(ADDR_TABLE_WRITER, (uintptr_t)&TableWriterGuardStub, 3)) {
                LOG_ERROR(kCat, "Failed to write table_writer JMP");
                return false;
            }
            LOG_INFO(kCat, "Hooked table_writer @ 0x%X (NULL-str guard) -> stub=%p tramp=%p",
                     (unsigned)ADDR_TABLE_WRITER, (void*)&TableWriterGuardStub, twTramp);
        } else {
            LOG_ERROR(kCat, "table_writer bytes MISMATCH! Expected 53 8B D9 57 8B 7C 24 0C");
            LOG_ERROR(kCat, "  Got: %02X %02X %02X %02X %02X %02X %02X %02X",
                     twBytes[0], twBytes[1], twBytes[2], twBytes[3],
                     twBytes[4], twBytes[5], twBytes[6], twBytes[7]);
            return false;
        }
        LOG_INFO(kCat, "TextReplacementFeature installed successfully (dict=%zu entries)",
                 g_dict.size());
        return true;
    }

    void OnUninstall() {
        g_chineseStrs.clear();
    }
};

REGISTER_FEATURE(TextReplacementFeature)
