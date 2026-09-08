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
// InsertLineBreaks: 在 UTF-8 中文译文中按字符边界插入 '\n'
//
// 背景: 引擎渲染层对长文本按固定字节宽度(~56B)切行，UTF-8 字符(3B)
//       会被从中间切碎 → 半截字符乱码。且超长行宽度计算溢出(INT_MAX
//       附近) → 行被画到屏幕外 → 简报"缺段"。
// 方案: 返回字典中文前，把长译文拆成 ≤51 字节(17 汉字)的行，行间插
//       '\n'。sub_48BDC0 遇 '\n' 自动换行(0x48BDFE)，引擎再切行时
//       切点落在 '\n' 上，不会切碎 UTF-8；且每行宽度 < 屏幕宽，溢出消失。
//
// 注意: 字典中文里已有 '\n' 的行保持原样；尾部 '\n' 会去除(避免空行)。
// ===================================================================
std::string InsertLineBreaks(const std::string& cn) {
    if (cn.empty()) return cn;
    std::string out;
    out.reserve(cn.size() + cn.size() / 8);
    int posBytes = 0;
    size_t i = 0, n = cn.size();
    while (i < n) {
        unsigned char b = (unsigned char)cn[i];
        int len;
        if (b < 0x80) len = 1;
        else if (b < 0xC0) len = 1;          // 孤立 continuation（异常数据，按单字节）
        else if (b < 0xE0) len = 2;
        else if (b < 0xF0) len = 3;
        else len = 4;

        if (len > 1 && i + len > n) len = 1; // 截断的 UTF-8 → 按单字节，避免越界

        if (b == '\n' || b == '\r') {
            // 字典自带换行: 提交当前行, 保留原换行
            out.append(cn, i, 1);
            posBytes = 0;
            i += 1;
            continue;
        }

        // 达到行宽上限(51B=17汉字): 插入换行
        if (posBytes > 0 && posBytes + len > 51) {
            out += '\n';
            posBytes = 0;
        }
        out.append(cn, i, len);
        posBytes += len;
        i += len;
    }
    // 去除尾部换行（避免末尾空行）
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
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
// ===================================================================
// GetStringGap: 计算当前字符串到下一字符串起点之间的可写字节数
//   this_ptr = text_lib_obj (sub_48DEA0 的 ecx)
//   index    = 当前字符串下标
//   orig     = 当前字符串指针 (数据区基址 + off[index])
//
//   sub_48DEA0 结构:
//     this+0x04 = count
//     this+0x0C = 偏移表容器, *(容器+8) = uint32_t* off_table
//     this+0x10 = 数据区容器, *(容器+8) = base
//     返回 = base + off_table[index]
//
//   空隙 = off_table[index+1] - off_table[index]
//   (当前字符串起点到下一字符串起点, 包含自身 NUL + 对齐填充)
//   返回 -1 表示无法计算 (最后一项 / 下一项无效 / 偏移表不可读) — 调用方回退保守策略
// =====================================================================
static int GetStringGap(void* this_ptr, uint32_t index, const char* orig) {
    if (!this_ptr || !orig) return -1;
    const uint8_t* t = (const uint8_t*)this_ptr;
    uint32_t count = *(const uint32_t*)(t + 0x04);
    if (index + 1 >= count) return -1;  // 最后一项: 无下一项可参照

    // 读取偏移表基址
    const uint32_t* offTableBase = *(const uint32_t* const*)(t + 0x0C);
    if (!offTableBase) return -1;
    const uint32_t* offTable = *(const uint32_t* const*)offTableBase;
    if (!offTable) return -1;

    // 读取数据区基址
    const uint32_t* dataBasePtr = *(const uint32_t* const*)(t + 0x10);
    if (!dataBasePtr) return -1;
    const char* dataBase = *(const char* const*)dataBasePtr;
    if (!dataBase) return -1;

    uint32_t curOff = offTable[index];
    uint32_t nextOff = offTable[index + 1];
    if (nextOff == 0xFFFFFFFFu) return -1;  // 下一项无效
    if (nextOff <= curOff) return -1;       // 非升序 → 不可靠，保守

    // 校验 orig 确实指向 数据区基址+curOff
    if ((const char*)dataBase + curOff != orig) return -1;

    return (int)(nextOff - curOff);
}

const char* OnTextLookup(void* this_ptr, uint32_t index) {
    g_callCount++;

    // 调用原始函数获取德语文本
    const char* orig = g_origTextLookup(this_ptr, index);

    if (!orig || !*orig) return orig;

    if (g_dict.empty()) return orig;

    // 查字典，命中时优先就地覆写；空隙不足则回退到字典堆指针
    auto it = g_dict.find(orig);
    if (it != g_dict.end()) {
        g_hitCount++;
        // 长译文先按 UTF-8 字符边界插入 '\n'（每行 ≤51B），
        // 防止引擎按字节切行切碎 UTF-8 / 超长行宽度溢出画到屏外。
        // 短译文(≤51B)原样返回，性能与行为不变。
        const std::string& cn0 = it->second;
        std::string cn;   // 排版后译文（短文本时为空 → 直接用 cn0）
        if (cn0.size() > 51)
            cn = InsertLineBreaks(cn0);
        const std::string& cnr = cn.empty() ? cn0 : cn;
        size_t cn_len = cnr.size();
        size_t orig_len = strlen(orig);

        // 尝试利用相邻条目空隙放宽覆写边界 (>= 原文长度 + 1 NUL)
        int gap = GetStringGap(this_ptr, index, orig);
        bool canInPlace = false;
        if (gap >= 0) {
            canInPlace = (cn_len + 1 <= (size_t)gap);
        } else {
            canInPlace = (cn_len <= orig_len);  // 保守回退: 原文空间内
        }

        if (canInPlace) {
            // 中文(含NUL)可以安全放入 (原空间或相邻空隙)
            memcpy((void*)orig, cnr.c_str(), cn_len + 1);
            if (g_hitCount <= 64) {
                LOG_INFO(kCat, "HIT [%d] in-place %zu bytes (gap=%d, orig=%zu)",
                         g_hitCount, cn_len, gap, orig_len);
            }
            return orig;
        }

        // 空隙不足: 返回字典存储的堆指针 (进程生命周期内有效) 并注册到
        // g_chineseStrs, 让 TextRenderer 按 UTF-8 CJK 渲染。
        // 注意: cnr 是局部 std::string (cn 排版结果或 cn0 字典项)。
        //   - cn 非空 → 排版结果必须持久: 存到堆上 (new) 并由 g_chineseStrs
        //     记录, 进程内不释放 (与旧 HEAP 分支一致)。
        //   - cn 为空 → cnr 即字典项 cn0, c_str() 生命周期同字典, 安全。
        const char* heapPtr = cnr.c_str();
        if (!cn.empty()) {
            char* heap = new char[cn.size() + 1];
            memcpy(heap, cn.c_str(), cn.size() + 1);
            heapPtr = heap;
        }
        g_chineseStrs.insert(heapPtr);
        if (g_hitCount <= 64) {
            LOG_INFO(kCat, "HEAP [%d] %zu bytes (gap=%d < %zu), ptr=%p",
                     g_hitCount, cn_len, gap, cn_len + 1, heapPtr);
        }
        return heapPtr;
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
