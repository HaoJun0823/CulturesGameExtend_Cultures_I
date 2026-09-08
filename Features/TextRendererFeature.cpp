// TextRendererFeature.cpp
// ===================================================================
// Cultures 1 (Gold) CJK 渲染 hook
//
// Hook 1: sub_48BF40 @ 0x48BF40 (单字符渲染)
//   __thiscall(ecx=DrawCtx, [esp+4]=byte), ret 4
//   前 6 字节: 56 8B F1 8B 4E 50
//     (push esi; mov esi,ecx; mov ecx,[esi+50h])
//   Trampoline: 6 字节 + E9 jmp 0x48BF46
//   5-byte JMP + 1 NOP 覆盖 6 字节窗口
//
//   策略:
//     ASCII (< 0x80): relay 到 trampoline（引擎原渲染）
//     CJK lead (0xC0-0xFF): 记录 (x,y)，缓冲码点，skip original
//     CJK continuation (0x80-0xBF): 累加码点，码点完整时
//       GDI 光栅化 → 8bpp 帧缓冲直写 → 推进 x，skip original
//
// Hook 2: sub_490030 @ 0x490030 (整串宽度)
//   __thiscall(ecx=font, [esp+4]=str), ret 4
//   前 5 字节: 53 8B 5C 24 08
//     (push ebx; mov ebx,[esp+8])
//   Trampoline: 5 字节 + E9 jmp 0x490035
//
//   策略:
//     纯 ASCII: relay
//     含 CJK: 逐字节，ASCII → 子_488A50 查表; CJK lead → fontSize;
//             continuation → 0
//
// DrawCtx 结构 (sub_48BF40 的 this/ecx):
//   +0x10 = 帧缓冲宽度边界 (int32)
//   +0x24 = 帧缓冲基址 (uint8_t*)
//   +0x30 = pitch (int32, 8bpp = bytes per row)
//   +0x50 = font 对象
//   +0x54 = color_ctx
//   +0x58 = 当前 x (int32)
//   +0x5C = 当前 y (int32)
//   +0x60 = 行起始 x
//   +0x64 = 居中格宽 (int32, 非零=居中模式)
//
// 8bpp 调色板模式:
//   帧缓冲是 8bpp，像素值=调色板索引。
//   引擎通过 color_ctx+4 指定调色板索引（sub_490090: font+3 = *(color_ctx+4)）。
//   阴影/高亮 pass 用不同 color_ctx，因此必须读取引擎颜色索引，
//   否则所有 pass 都画成同色 → "文字出现两次"。
// ===================================================================

#include "pch.h"
#include "Core/Feature.h"
#include "Core/Patch.h"
#include "Core/Logger.h"
#include "Core/GdiFont.h"
#include "Core/IniConfig.h"
#include "Core/GameVersion.h"
#include "Core/Paths.h"

#include <intrin.h>   // _ReturnAddress
#include <atomic>
#include <cstring>
#include <unordered_set>
#include <unordered_map>

// ---- Chinese string pointer set (shared with TextReplacementFeature) ----
// TextReplacementFeature inserts returned c_str() pointers here.
// TextRendererFeature queries this to decide UTF-8 vs Latin-1 rendering.
std::unordered_set<const char*> g_chineseStrs;

// ---- Per-string CJK flag (set by sub_48BDC0 hook) ----
// Replaces the old global g_utf8Mode. Only true when rendering a string
// whose pointer is in g_chineseStrs.
static bool s_renderingChinese = false;

// Check if string pointer is in our Chinese set OR contains valid UTF-8 CJK.
// The pointer check may fail because callers copy the string to a temp buffer
// before rendering; in that case we fall back to content-based detection.
static bool IsChineseStr(const char* s) {
    if (!s) return false;
    // Fast path: pointer match
    if (g_chineseStrs.count(s) > 0) return true;
    // Fallback: scan for valid UTF-8 sequences with CJK codepoints
    // CJK Unified Ideographs: U+4E00–U+9FFF → UTF-8: E4-E9 ...
    // We just check: any byte >= 0xC0 that starts a valid UTF-8 sequence
    // decoding to U+0080 or above (i.e. not ASCII-compatible).
    // This is safe because CP1252/German text never has valid multi-byte UTF-8
    // sequences (bytes 0x80-0xBF as continuations only occur after 0xC0+ leads,
    // and German umlauts like ä=0xE4, ö=0xF6, ü=0xFC are single bytes in CP1252
    // that happen to look like UTF-8 lead bytes).
    // More precise: check for 3-byte UTF-8 sequences (E0-EF xx xx) where
    // continuation bytes are in 0x80-0xBF range. CP1252 German text won't have
    // two consecutive bytes in 0x80-0xBF after a 0xE0-0xEF lead.
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p >= 0xE0 && *p <= 0xEF) {
            // Potential 3-byte UTF-8 lead
            if (p[1] >= 0x80 && p[1] <= 0xBF &&
                p[2] >= 0x80 && p[2] <= 0xBF) {
                return true;  // Valid UTF-8 CJK sequence found
            }
        }
        if (*p >= 0xF0 && *p <= 0xF4) {
            // Potential 4-byte UTF-8 lead (emoji etc.)
            if (p[1] >= 0x80 && p[1] <= 0xBF &&
                p[2] >= 0x80 && p[2] <= 0xBF &&
                p[3] >= 0x80 && p[3] <= 0xBF) {
                return true;
            }
        }
    }
    return false;
}

namespace {

const char* kCat = "[TextRenderer]";

// ---- Game addresses (Cultures.exe, image base 0x400000) ----
constexpr uintptr_t ADDR_CHAR_RENDER  = 0x48BF40;  // sub_48BF40 (ret 4)
constexpr uintptr_t ADDR_FONT_QUERY_STRING = 0x490030;  // sub_490030 (ret 4)
constexpr uintptr_t ADDR_FRAME_TABLE_LOOKUP = 0x488A50;  // sub_488A50 (ret 4)
constexpr uintptr_t ADDR_STRING_RENDER = 0x48BDC0;  // sub_48BDC0 (string iterator)

// ---- Trampolines (store as function pointers for indirect jmp) ----
void (*g_trampCharRender)() = nullptr;
void (*g_trampQueryString)() = nullptr;
void (*g_trampStringRender)() = nullptr;

// ---- GDI rasterizer ----
ge::text::GdiFontRasterizer* g_rasterizer = nullptr;

// ---- Font config ----
std::wstring g_fontName = L"Microsoft YaHei";
int  g_fontSize   = 16;
int  g_fontWeight = 400;
bool g_antiAlias  = true;
uint32_t g_textColor = 0xFFFFFF;

// ---- Render state ----
bool g_ready = false;

// ---- UTF-8 state machine (single-threaded text rendering) ----
static int      s_need = 0;    // continuation bytes needed
static int      s_got  = 0;    // continuation bytes received
static uint32_t s_cp   = 0;    // codepoint accumulator
static int      s_lx   = 0;    // lead byte 时的 x
static int      s_ly   = 0;    // lead byte 时的 y

// ---- 前景色调色板索引 (8bpp) ----
// C1 调色板: 0xFF 通常是白色。可配置。
uint8_t g_fgIndex = 0xFF;

// ---- Y 偏移微调 ----
int g_yOffset = 0;

// ---- Debug ----
static bool s_loggedFirst = false;
static bool s_loggedFirstSR = false;
static int  s_srLogCount = 0;  // string_render 日志计数
static int  s_chLogCount = 0;  // char_render 日志计数

// ---- 去重日志：记录每个 (str,x0,y,fb) 组合首次出现 ----
// 旧的 1000 条上限会截断日志，可能漏掉右侧重影的记录。
// 去重后每个绘制位置只记 1 条，覆盖全量且不刷爆日志。
static std::unordered_set<std::string> s_srSeen;

// ===================================================================
// 简单单通道渲染（回退方案）
//
// 早期"批次合并"假设引擎对同一字符串做"4 偏移阴影 + 1 中心主色"共 5 个
// pass，于是缓存成组、只画主色一遍。但实测日志证明：引擎对同一菜单项
// 以完全相同的 (x,y) 反复调用 sub_48BDC0 多达几十次，且 colorIdx 相同，
// 并无偏移阴影——批次模型错误，导致同一串被叠加渲染 N 遍（更模糊、更重影）。
//
// 现回退为：OnStringRender 对每个调用直接 GDI 渲染一遍并推进 x，返回
// handled 跳过原引擎字符串迭代器。基线清晰后再分析"同位置重复"的真实时序。
// ===================================================================

// ===================================================================
// Helper: read engine color index from color_ctx (DrawCtx+0x54)
// Returns -1 if unavailable (caller falls back to g_fgIndex).
// (Moved before GetDisplayInfo which calls it.)
// ===================================================================
static int GetEngineColorIndex(const void* drawCtx) {
    if (!drawCtx) return -1;
    const void* colorCtx = *(const void* const*)((const char*)drawCtx + 0x54);
    if (!colorCtx) return -1;
    return *(const int32_t*)((const char*)colorCtx + 4);
}

// ===================================================================
// Display info: derive bpp/pitch_bytes/fbW/fbH from DrawCtx fields.
//
// REVERTED TO KNOWN-GOOD LOGIC (172544 normal version):
//   fb   = *(DrawCtx + 0x24)
//   pitch = *(DrawCtx + 0x30)  (pixel width; byte pitch = pitch * 2 for 16bpp)
//   bpp  = 2 (fixed 16bpp RGB565)
//   fbH  = 2048 (hardcoded; engine never reads this for clipping in 16bpp mode)
//
// This is the configuration that produced correct main-menu rendering.
// In-game rendering issues will be diagnosed separately via log analysis.
// ===================================================================
struct DisplayInfo {
    uint8_t* fb;
    int pitchBytes;
    int bpp;
    int fbW;
    int fbH;
    uint8_t paletteIndex;  // 8bpp: palette index; 16bpp: unused
};

static bool GetDisplayInfo(void* drawCtx, DisplayInfo& info) {
    if (!drawCtx) return false;
    char* ctx = (char*)drawCtx;

    // Read from DrawCtx using original known-good offsets
    info.fb = *(uint8_t**)(ctx + 0x24);
    int pitch_px = *(int32_t*)(ctx + 0x30);
    if (!info.fb || pitch_px <= 0) return false;

    // Fixed 16bpp RGB565 (known-good configuration)
    info.bpp = 2;
    info.pitchBytes = pitch_px * 2;
    info.fbW = pitch_px;
    info.fbH = 2048;
    info.paletteIndex = g_fgIndex;

    return true;
}

// ===================================================================
// Helper: render a single glyph via GDI into framebuffer (8/16/32bpp)
// Uses GdiFontRasterizer::BlitGlyph which supports 16bpp RGB565.
// Updates x in-place.
// ===================================================================
static void RenderGlyph(uint32_t cp, uint8_t* fb, int pitchBytes, int bpp,
                        int& x, int y, int cellW, int fbW, int fbH,
                        uint32_t color, bool skipBlit) {
    const ge::text::Glyph* g = g_rasterizer->GetGlyph(cp);
    if (!g || g->w == 0 || g->h == 0) {
        x += (cellW != 0) ? cellW : g_fontSize;
        return;
    }
    int dx, advance;
    if (cellW != 0) {
        dx = x + (cellW - g->advance) / 2;
        advance = cellW;
    } else {
        dx = x;
        advance = g->advance;
    }
    int dy = y + g_yOffset + g_rasterizer->Ascent() + g->originY;
    dx += g->originX;
    if (!skipBlit)
        ge::text::GdiFontRasterizer::BlitGlyph(fb, pitchBytes, bpp, dx, dy, g, color,
                                      fbW, fbH, true, -1, -1, -1, -1);
    x += advance;
}

// ===================================================================
// OnCharRender: C handler for sub_48BF40 hook
//   drawCtx = DrawCtx (ecx 原始值)
//   byte = 字节
//   Returns true = handled (skip original), false = relay
// ===================================================================
bool OnCharRender(void* drawCtx, uint8_t byte) {
    if (!drawCtx) return false;

    // 读引擎颜色索引
    int engineColorIdx = GetEngineColorIndex(drawCtx);
    uint8_t fgIndex = (engineColorIdx >= 0 && engineColorIdx <= 0xFF)
                          ? (uint8_t)engineColorIdx : g_fgIndex;

    // First-call debug log
    if (!s_loggedFirst) {
        s_loggedFirst = true;
        LOG_INFO(kCat, "OnCharRender FIRST: byte=0x%02X drawCtx=%p ready=%d colorIdx=%d",
                 byte, drawCtx, g_ready ? 1 : 0, engineColorIdx);
    }

    // ASCII: relay to engine
    if (byte < 0x80) {
        // BUGFIX: ASCII byte while expecting UTF-8 continuation → reset state
        if (s_need > 0) {
            s_need = 0; s_got = 0; s_cp = 0;
        }
        return false;
    }

    // 非 UTF-8 模式：所有 ≥0x80 字节 relay（Latin-1 德语不受影响）
    if (!s_renderingChinese) return false;

    if (!g_ready || !g_rasterizer) return false;

    // ---- UTF-8 lead byte ----
    if (byte >= 0xC0) {
        s_cp = 0;
        if (byte >= 0xF0)      { s_need = 3; s_cp = byte & 0x07; }
        else if (byte >= 0xE0) { s_need = 2; s_cp = byte & 0x0F; }
        else                    { s_need = 1; s_cp = byte & 0x1F; }
        s_got = 0;
        s_lx = *(int32_t*)((char*)drawCtx + 0x58);
        s_ly = *(int32_t*)((char*)drawCtx + 0x5C);
        if (s_chLogCount < 500) {
            s_chLogCount++;
            LOG_INFO(kCat, "UTF-8 lead: byte=0x%02X need=%d x=%d y=%d ctx=%p",
                     byte, s_need, s_lx, s_ly, drawCtx);
        }
        return true;  // skip original, wait for continuation
    }

    // ---- UTF-8 continuation byte ----
    if (byte >= 0x80 && byte <= 0xBF) {
        if (s_need == 0) {
            // 没在缓冲（孤立 continuation）→ relay
            return false;
        }
        s_cp = (s_cp << 6) | (byte & 0x3F);
        s_got++;
        if (s_got < s_need) return true;  // 还需更多 continuation

        // ---- 码点完整，渲染 ----
        uint32_t cp = s_cp;
        s_need = 0; s_got = 0; s_cp = 0;

        const ge::text::Glyph* g_glyph = g_rasterizer->GetGlyph(cp);
        if (!g_glyph || g_glyph->w == 0 || g_glyph->h == 0) {
            LOG_INFO(kCat, "Skip cp=U+%04X (no glyph)", cp);
            return true;
        }

        // 从 DrawCtx 读帧缓冲信息（支持 16bpp）
        DisplayInfo di;
        if (!GetDisplayInfo(drawCtx, di)) {
            LOG_WARN(kCat, "Invalid display info for drawCtx=%p", drawCtx);
            return true;
        }

        int      x     = s_lx;
        int      y     = s_ly;
        int      cellW = *(int32_t*)((char*)drawCtx + 0x64);

        // 计算渲染位置
        int dx, advance;
        if (cellW != 0) {
            // 居中模式
            dx = x + (cellW - g_glyph->advance) / 2;
            advance = cellW;
        } else {
            dx = x;
            advance = g_glyph->advance;
        }

        // GDI 基线对齐: originY 是相对基线的偏移（通常负值=向上）
        // C1 的 y 是行起始位置，需要加 ascent 到基线
        int dy = y + g_yOffset + g_rasterizer->Ascent() + g_glyph->originY;
        dx += g_glyph->originX;

        // 8bpp: pass palette index; 16bpp: pass RGB
        uint32_t renderColor = (di.bpp == 1) ? (uint32_t)di.paletteIndex : g_textColor;
        ge::text::GdiFontRasterizer::BlitGlyph(di.fb, di.pitchBytes, di.bpp, dx, dy,
                                      g_glyph, renderColor, di.fbW, di.fbH, true,
                                      -1, -1, -1, -1);

        // 推进 x
        *(int32_t*)((char*)drawCtx + 0x58) = x + advance;

        if (s_chLogCount < 500) {
            s_chLogCount++;
            LOG_INFO(kCat, "CharRender cp=U+%04X dx=%d dy=%d w=%d h=%d adv=%d ctx=%p x0=%d y0=%d cellW=%d",
                     cp, dx, dy, g_glyph->w, g_glyph->h, advance,
                     drawCtx, x, y, cellW);
        }

        return true;  // skip original
    }

    return false;  // relay
}

// ===================================================================
// OnQueryString: C handler for sub_490030 hook
//   font = font 对象 (ecx 原始值)
//   str = 字符串
//   Returns >= 0 = total width (handled), -1 = relay
// ===================================================================
int OnQueryString(const void* font, const char* str) {
    if (!font || !str) return -1;
    if (!g_ready || !g_rasterizer) return -1;
    if (!IsChineseStr(str)) return -1;  // Only intercept Chinese strings

    // Check for CJK bytes
    bool hasCJK = false;
    for (const char* p = str; *p; p++) {
        if ((uint8_t)*p >= 0x80) { hasCJK = true; break; }
    }
    if (!hasCJK) return -1;  // Pure ASCII relay

    // Decode UTF-8 and sum GDI advances for all characters (including ASCII)
    // to match OnStringRender's GDI-based rendering width.
    size_t slen = strlen(str);
    auto cps = ge::text::GdiFontRasterizer::DecodeUtf8(str, slen);
    int total = 0;
    for (uint32_t cp : cps) {
        const ge::text::Glyph* g = g_rasterizer->GetGlyph(cp);
        total += g ? g->advance : g_fontSize;
    }
    return total;
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
// Naked stubs
//
// sub_48BF40: __thiscall(ecx=DrawCtx, [esp+4]=byte), ret 4
//   stub: 暂存 ecx→ebx, 调 OnCharRender, true→ret 4, false→relay
//
// sub_49000: __thiscall(ecx=font, [esp+4]=str), ret 4
//   stub: 暂存 ecx→ebx, 调 OnQueryString, >=0→eax+ret 4, -1→relay
//
// relay 分支: 恢复 ecx=ebx, pop ebx, jmp trampoline
//   (trampoline 执行被覆盖的原始字节然后 jmp entry+copyLen)
// ===================================================================

extern "C" void __declspec(naked) CharRenderStub() {
    __asm {
        // 入口: ecx=DrawCtx, [esp+4]=byte
        push ebx
        mov ebx, ecx               // ebx = DrawCtx (非易失)
        movzx eax, byte ptr [esp+8] // byte (push ebx 后偏移 +4)
        push eax                    // arg2: byte
        push ebx                    // arg1: drawCtx
        call OnCharRender
        add esp, 8
        test al, al
        jz char_relay
        pop ebx
        ret 4
    char_relay:
        mov ecx, ebx               // 恢复 ecx = DrawCtx
        pop ebx                     // 恢复 ebx
        jmp dword ptr [g_trampCharRender]
    }
}

extern "C" void __declspec(naked) QueryStringStub() {
    __asm {
        // 入口: ecx=font, [esp+4]=str
        push ebx
        mov ebx, ecx               // ebx = font
        push dword ptr [esp+8]     // str (push ebx 后偏移 +4)
        push ebx                    // font
        call OnQueryString
        add esp, 8
        cmp eax, -1
        jne qs_done
        // relay
        mov ecx, ebx               // 恢复 ecx = font
        pop ebx                     // 恢复 ebx
        jmp dword ptr [g_trampQueryString]
    qs_done:
        pop ebx
        ret 4
    }
}

// ===================================================================
// RenderStringCore: 把字符串渲染到指定帧缓冲（GDI 全量）
// 不修改 DrawCtx 的 x/y（由调用方负责）。返回结束时的 (x,y)。
// 支持 8/16/32bpp（由 bpp 参数决定）。
// ===================================================================
static void RenderStringCore(uint8_t* fb, int pitchBytes, int bpp,
                             int fbW, int fbH,
                             int x, int y,
                             int rowStartX, int cellW, void* font,
                             const char* str, uint32_t color, bool skipBlit,
                             int& outX, int& outY) {
    const unsigned char* p = (const unsigned char*)str;
    while (*p) {
        uint8_t b = *p++;

        switch (b) {
            case 0:
                goto core_done;
            case 0x09:  // tab
            case 0x20:  // space
            case 0x5F:  // underscore (treated as space by engine)
                if (cellW != 0) {
                    x += cellW;
                } else {
                    const ge::text::Glyph* sg = g_rasterizer->GetGlyph(0x20);
                    x += sg ? sg->advance : (g_fontSize / 2);
                }
                break;
            case 0x0A:  // LF
            case 0x0D:  // CR
                if (*p == 0x0A) p++;  // CRLF -> skip LF
                x = rowStartX;
                if (font) {
                    int16_t lineH = *(int16_t*)((char*)font + 6);
                    y += lineH + 2;
                } else {
                    y += g_fontSize + 2;
                }
                break;
            default:
                if (b < 0x20) break;  // ignore other control chars
                if (b < 0x80) {
                    RenderGlyph(b, fb, pitchBytes, bpp, x, y, cellW, fbW, fbH, color, skipBlit);
                } else if (b >= 0xC0) {
                    int len; uint32_t cp;
                    if (b >= 0xF0)      { len = 4; cp = b & 0x07; }
                    else if (b >= 0xE0) { len = 3; cp = b & 0x0F; }
                    else                 { len = 2; cp = b & 0x1F; }

                    bool valid = true;
                    for (int k = 1; k < len; k++) {
                        uint8_t cb = p[k - 1];
                        if ((cb & 0xC0) != 0x80) { valid = false; break; }
                        cp = (cp << 6) | (cb & 0x3F);
                    }
                    if (!valid) break;
                    p += len - 1;

                    RenderGlyph(cp, fb, pitchBytes, bpp, x, y, cellW, fbW, fbH, color, skipBlit);
                }
                break;
        }
    }

core_done:
    outX = x;
    outY = y;
}

// ===================================================================
// OnStringRender: C handler for sub_48BDC0 hook (FULL TAKEOVER)
//   drawCtx = ecx (DrawCtx)
//   str = [esp+4] (the string being rendered)
//   Returns: 0 = relay (not Chinese), 1 = handled (skip original entirely)
//
// 简单单通道：对每个调用直接用 GDI 渲染整串并推进 DrawCtx 的 x，
// 返回 1 跳过原 sub_48BDC0。
// ===================================================================
int OnStringRender(void* drawCtx, const char* str) {
    if (!drawCtx || !str) return 0;

    bool isChinese = IsChineseStr(str);

    if (!s_loggedFirstSR) {
        s_loggedFirstSR = true;
        LOG_INFO(kCat, "OnStringRender FIRST: str='%.30s' chinese=%d", str, isChinese ? 1 : 0);
    }

    if (!isChinese) {
        s_renderingChinese = false;
        return 0;  // relay to engine
    }

    s_renderingChinese = true;

    if (!g_ready || !g_rasterizer) return 0;  // fallback to engine

    char* ctx = (char*)drawCtx;

    // ---- 读取显示信息（bpp/pitchBytes/fbW/fbH）----
    DisplayInfo di;
    if (!GetDisplayInfo(drawCtx, di)) {
        LOG_WARN(kCat, "OnStringRender: invalid display info");
        return 0;  // fallback to engine
    }

    int      x         = *(int32_t*)(ctx + 0x58);
    int      y         = *(int32_t*)(ctx + 0x5C);
    int      rowStartX = *(int32_t*)(ctx + 0x60);
    int      cellW     = *(int32_t*)(ctx + 0x64);
    void*    font      = *(void**)(ctx + 0x50);

    // ---- 5-pass 去重：同字符串在同一 fb 上的连续多次调用，只画一次 ----
    // C1 引擎对每个菜单项会调用 sub_48BDC0 最多 5 次：4 个 ±1px 偏移阴影
    // pass + 1 个中心 pass。若每次都画整串白字，5 层白字在不同位置叠加
    // → 文字变模糊（与初始"文字模糊"症状吻合）。
    // 方案：同一字符串在相邻位置连续出现时，只画第一次，后续 pass 只度量
    // 宽度（推进 DrawCtx.x），不实际 blit。
    static const char* s_lastStr = nullptr;
    static uintptr_t   s_lastFb  = 0;
    static int         s_lastX   = 0;
    static int         s_lastY   = 0;
    bool skipBlit = false;
    if (s_lastStr == str && s_lastFb == (uintptr_t)di.fb &&
        abs(x - s_lastX) <= 2 && abs(y - s_lastY) <= 2) {
        skipBlit = true;  // 重复 pass：只度量，不画
    } else {
        s_lastStr = str;
        s_lastFb  = (uintptr_t)di.fb;
        s_lastX   = x;
        s_lastY   = y;
    }

    // ---- 直接渲染整串，推进 x（skipBlit 时只度量宽度）----
    // 8bpp: pass palette index as color; 16bpp: pass RGB
    uint32_t renderColor = (di.bpp == 1) ? (uint32_t)di.paletteIndex : g_textColor;
    int outX, outY;
    RenderStringCore(di.fb, di.pitchBytes, di.bpp, di.fbW, di.fbH,
                     x, y, rowStartX, cellW, font, str, renderColor, skipBlit,
                     outX, outY);
    *(int32_t*)(ctx + 0x58) = outX;
    *(int32_t*)(ctx + 0x5C) = outY;

    // ---- 去重日志 ----
    {
        char keyBuf[128];
        _snprintf_s(keyBuf, sizeof(keyBuf), _TRUNCATE, "%s|%d|%d|%p", str, x, y, di.fb);
        if (s_srSeen.insert(std::string(keyBuf)).second) {
            s_srLogCount++;
            LOG_INFO(kCat, "OnStringRender draw: str='%.16s' x0=%d y=%d bpp=%d pitchBytes=%d fbW=%d fbH=%d color=0x%06X ctx=%p fb=%p cellW=%d palIdx=%d",
                     str, x, y, di.bpp, di.pitchBytes, di.fbW, di.fbH, renderColor,
                     drawCtx, di.fb, cellW, di.paletteIndex);
        }
    }

    return 1;  // handled: skip original sub_48BDC0
}
        extern "C" void __declspec(naked) StringRenderStub() {
    __asm {
        // 入口: ecx=DrawCtx, [esp+4]=str  (fastcall: edx unused, retn 4)
        push ebx
        mov ebx, ecx               // ebx = DrawCtx
        push dword ptr [esp+8]     // str (push ebx 后偏移 +4)
        push ebx                    // drawCtx
        call OnStringRender
        add esp, 8
        test eax, eax
        jnz sr_skip                 // non-zero = handled, skip original
        // relay: restore ecx and jump to trampoline
        mov ecx, ebx
        pop ebx
        jmp dword ptr [g_trampStringRender]
    sr_skip:
        pop ebx
        ret 4                      // skip original, clean 1 stack arg
    }
}

} // namespace

// ===================================================================
// Feature class
// ===================================================================
class TextRendererFeature : public Feature {
public:
    const char* GetName() const override { return "TextRenderer"; }
    GameTarget GetTarget() const override { return GameTarget::Cultures; }

    bool OnInstall(IniConfig& cfg, GameVersion& ver) override {
        // ---- Read font configuration ----
        std::string fontName = cfg.GetString("TextRenderer", "FontName", "Microsoft YaHei");
        g_fontSize   = cfg.GetInt("TextRenderer", "FontSize", 16);
        g_fontWeight = cfg.GetInt("TextRenderer", "FontWeight", 400);
        g_antiAlias  = cfg.GetBool("TextRenderer", "AntiAlias", true);

        // Text color (0xRRGGBB)
        std::string colorStr = cfg.GetString("TextRenderer", "TextColor", "0xFFFFFF");
        if (colorStr.size() > 2 && colorStr[0] == '0' && (colorStr[1] == 'x' || colorStr[1] == 'X'))
            colorStr = colorStr.substr(2);
        g_textColor = (uint32_t)strtoul(colorStr.c_str(), nullptr, 16);
        if (g_textColor == 0) g_textColor = 0xFFFFFF;

        // Foreground palette index (8bpp)
        g_fgIndex = (uint8_t)cfg.GetInt("TextRenderer", "FgIndex", 0xFF);

        // Y offset fine-tune
        g_yOffset = cfg.GetInt("TextRenderer", "YOffset", 0);

        // Convert font name to wide string
        int wlen = MultiByteToWideChar(CP_UTF8, 0, fontName.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            g_fontName.resize(wlen);
            MultiByteToWideChar(CP_UTF8, 0, fontName.c_str(), -1, &g_fontName[0], wlen);
        }

        // ---- Initialize GDI rasterizer ----
        g_rasterizer = new ge::text::GdiFontRasterizer();
        if (!g_rasterizer->Create(g_fontName.c_str(), g_fontSize, g_fontWeight, false, g_antiAlias)) {
            LOG_ERROR(kCat, "Failed to create GDI font '%s' size=%d", fontName.c_str(), g_fontSize);
            delete g_rasterizer;
            g_rasterizer = nullptr;
            return false;
        }
        g_ready = true;
        LOG_INFO(kCat, "GDI font created: '%s' size=%d weight=%d aa=%d color=0x%06X fgIdx=%d yOffset=%d",
                 fontName.c_str(), g_fontSize, g_fontWeight, (int)g_antiAlias, g_textColor, g_fgIndex, g_yOffset);
        LOG_INFO(kCat, "Font metrics: ascent=%d descent=%d",
                 g_rasterizer->Ascent(), g_rasterizer->Descent());

        // ---- Hook 1: sub_48BF40 (char render) ----
        // 还原 9841A43C 正常版：默认禁用此 hook。所有文本由 OnStringRender
        // 全量接管渲染；char_render 拦截会与全量绘制叠加（双重绘制 → 色彩诡异）。
        // 仅诊断需要时可用 [TextRenderer] EnableCharHook=1 临时开启。
        if (cfg.GetBool("TextRenderer", "EnableCharHook", false)) {
            auto bytes = Patch::ReadBytes(ADDR_CHAR_RENDER, 8);
            if (bytes.size() >= 6 &&
                bytes[0] == 0x56 && bytes[1] == 0x8B && bytes[2] == 0xF1 &&
                bytes[3] == 0x8B && bytes[4] == 0x4E && bytes[5] == 0x50) {
                LOG_INFO(kCat, "char_render bytes OK: %02X %02X %02X %02X %02X %02X",
                         bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);

                g_trampCharRender = (void(*)())MakeTrampoline(ADDR_CHAR_RENDER, 6);
                if (!g_trampCharRender) {
                    LOG_ERROR(kCat, "Failed to create char_render trampoline");
                    return false;
                }
                if (!Patch::WriteJmp(ADDR_CHAR_RENDER, (uintptr_t)&CharRenderStub, 1)) {
                    LOG_ERROR(kCat, "Failed to write char_render JMP");
                    return false;
                }
                LOG_INFO(kCat, "Hooked char_render @ 0x%X -> stub=%p tramp=%p",
                         (unsigned)ADDR_CHAR_RENDER,
                         (void*)&CharRenderStub, (void*)g_trampCharRender);
            } else {
                LOG_ERROR(kCat, "char_render bytes MISMATCH! Expected 56 8B F1 8B 4E 50");
                return false;
            }
        }

        // ---- Hook 2: font_query_string (0x490030) ----
        if (cfg.GetBool("TextRenderer", "EnableQueryHook", false))
        {
            auto bytes = Patch::ReadBytes(ADDR_FONT_QUERY_STRING, 8);
            // Expected: 53 8B 5C 24 08
            if (bytes.size() >= 5 &&
                bytes[0] == 0x53 && bytes[1] == 0x8B &&
                bytes[2] == 0x5C && bytes[3] == 0x24 && bytes[4] == 0x08) {
                LOG_INFO(kCat, "font_query_string bytes OK: %02X %02X %02X %02X %02X",
                         bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

                g_trampQueryString = (void(*)())MakeTrampoline(ADDR_FONT_QUERY_STRING, 5);
                if (!g_trampQueryString) {
                    LOG_ERROR(kCat, "Failed to create query_string trampoline");
                    return false;
                }
                if (!Patch::WriteJmp(ADDR_FONT_QUERY_STRING, (uintptr_t)&QueryStringStub)) {
                    LOG_ERROR(kCat, "Failed to write query_string JMP");
                    return false;
                }
                LOG_INFO(kCat, "Hooked font_query_string @ 0x%X -> stub=%p tramp=%p",
                         (unsigned)ADDR_FONT_QUERY_STRING,
                         (void*)&QueryStringStub, (void*)g_trampQueryString);
            } else {
                LOG_ERROR(kCat, "font_query_string bytes MISMATCH! Expected 53 8B 5C 24 08");
                return false;
            }
        }

        // ---- Hook 3: sub_48BDC0 (string iterator) ----
        if (cfg.GetBool("TextRenderer", "EnableStringHook", false))
        {
            // Entry bytes: 55 56 57 8B 7C 24 10 (push ebp; push esi; push edi; mov edi,[esp+10h])
            // = 7 bytes (3+4). 5-byte JMP covers bytes 0-4, byte 5-6 (8B 7C) partially.
            // We copy the full 7-byte instruction window.
            auto bytes = Patch::ReadBytes(ADDR_STRING_RENDER, 12);
            // Expected: 55 56 57 8B 7C 24 10
            if (bytes.size() >= 7 &&
                bytes[0] == 0x55 && bytes[1] == 0x56 && bytes[2] == 0x57 &&
                bytes[3] == 0x8B && bytes[4] == 0x7C && bytes[5] == 0x24 &&
                bytes[6] == 0x10) {
                LOG_INFO(kCat, "string_render bytes OK: %02X %02X %02X %02X %02X %02X %02X",
                         bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6]);

                g_trampStringRender = (void(*)())MakeTrampoline(ADDR_STRING_RENDER, 7);
                if (!g_trampStringRender) {
                    LOG_ERROR(kCat, "Failed to create string_render trampoline");
                    return false;
                }
                // 5-byte JMP + 2 NOP (cover 7-byte window)
                if (!Patch::WriteJmp(ADDR_STRING_RENDER, (uintptr_t)&StringRenderStub, 2)) {
                    LOG_ERROR(kCat, "Failed to write string_render JMP");
                    return false;
                }
                LOG_INFO(kCat, "Hooked string_render @ 0x%X -> stub=%p tramp=%p",
                         (unsigned)ADDR_STRING_RENDER,
                         (void*)&StringRenderStub, (void*)g_trampStringRender);
            } else {
                LOG_ERROR(kCat, "string_render bytes MISMATCH! Expected 55 56 57 8B 7C 24 10");
                if (bytes.size() >= 7) {
                    LOG_ERROR(kCat, "  Got: %02X %02X %02X %02X %02X %02X %02X",
                             bytes[0], bytes[1], bytes[2], bytes[3],
                             bytes[4], bytes[5], bytes[6]);
                }
                return false;
            }
        }

        LOG_INFO(kCat, "TextRendererFeature installed successfully");
        return true;
    }

    void OnUninstall() {
        // Restore original bytes (not implemented; would need to save/restore)
        g_ready = false;
        if (g_rasterizer) {
            delete g_rasterizer;
            g_rasterizer = nullptr;
        }
    }
};

REGISTER_FEATURE(TextRendererFeature)