// GdiFont.cpp
// 见 GdiFont.h 的设计说明。本文件实现 GDI 光栅化与混合。
//
// 字形光栅化采用 DIB + TextOutW 路线（而非 GetGlyphOutline）：在无显示会话
// （服务/沙盒）里 GetGlyphOutline 的灰度缓冲格式容易崩溃，而 TextOut 到内存
// DIB 是最稳健、兼容性最好的取字方式，同样"从 TTF 动态生成到内存"。
// GdiFont.cpp
// See the design notes in GdiFont.h. This file implements the GDI rasterization
// and blending.
//
// Glyph rasterization uses the DIB + TextOutW path (rather than
// GetGlyphOutline): in a session without a display (service / sandbox),
// GetGlyphOutline's grayscale buffer format is crash-prone, whereas TextOut to
// an in-memory DIB is the most robust and most compatible way to fetch glyphs,
// and it too "dynamically generates from TTF into memory".
#include "pch.h"
#include "GdiFont.h"
#include <cstring>
#include <cstdio>


namespace ge {
namespace text {


#pragma region "Color helpers (RGB565)"
namespace {


inline uint16_t To565(uint32_t c) {
    uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
inline void From565(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = (uint8_t)(((v >> 11) & 0x1F) << 3);
    g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
    b = (uint8_t)((v & 0x1F) << 3);
}


}
// namespace
// namespace
GdiFontRasterizer::~GdiFontRasterizer() {

#pragma endregion

#pragma region "Lifecycle (dtor / Reset)"
    Reset();
}
void GdiFontRasterizer::Reset() {


    if (m_hbmp)  { if (m_hdc) SelectObject(m_hdc, GetStockObject(SYSTEM_FONT)); DeleteObject(m_hbmp); m_hbmp = nullptr; }
    if (m_hfont) { DeleteObject(m_hfont); m_hfont = nullptr; }
    if (m_hdc)   { DeleteDC(m_hdc); m_hdc = nullptr; }
    if (m_fontRes && !m_fontFile.empty()) {
        RemoveFontResourceExW(m_fontFile.c_str(), FR_PRIVATE, 0);
    }
    m_fontRes = 0; m_fontFile.clear();
    m_bits = nullptr; m_height = 0; m_ascent = 0; m_descent = 0;
    m_cellW = m_cellH = m_baseY = 0;
    m_cache.clear();
}
std::wstring GdiFontRasterizer::ReadTtfFamilyName(const wchar_t* path) {

// 读 TTF name 表，取族名（nameID 1/4/6）。失败返回空串。
// 仅依赖 windows.h，纯手工解析 TrueType 偏移表 + name 表。
#pragma endregion

#pragma region "ReadTtfFamilyName"
// Read the TTF name table and extract the family name (nameID 1/4/6). Returns an
// empty string on failure. Depends only on windows.h; manually parses the
// TrueType offset table + name table by hand.
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD sz = GetFileSize(h, nullptr);
    if (sz < 12) { CloseHandle(h); return {}; }
    std::vector<uint8_t> buf(sz);
    DWORD rd = 0;
    ReadFile(h, buf.data(), sz, &rd, nullptr);
    CloseHandle(h);
    if (rd < 12) return {};
    uint16_t numTables = (uint16_t)((buf[4] << 8) | buf[5]);


    uint32_t nameOff = 0;
    for (uint16_t i = 0; i < numTables; ++i) {
        uint32_t base = 12u + (uint32_t)i * 16u;
        if (buf[base] == 'n' && buf[base+1] == 'a' && buf[base+2] == 'm' && buf[base+3] == 'e') {
            nameOff = ((uint32_t)buf[base+8] << 24) | ((uint32_t)buf[base+9] << 16) |
                      ((uint32_t)buf[base+10] << 8) | (uint32_t)buf[base+11];
            break;
        }
    }
    if (!nameOff || nameOff + 6 > buf.size()) return {};
    uint16_t count  = (uint16_t)((buf[nameOff+2] << 8) | buf[nameOff+3]);


    uint16_t strOff = (uint16_t)((buf[nameOff+4] << 8) | buf[nameOff+5]);
    uint32_t so = nameOff + strOff;
    std::wstring best;
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t rb = nameOff + 6u + (uint32_t)i * 12u;
        if (rb + 12 > buf.size()) break;
        uint16_t pid = (uint16_t)((buf[rb] << 8) | buf[rb+1]);
        uint16_t nid = (uint16_t)((buf[rb+6] << 8) | buf[rb+7]);
        uint16_t len = (uint16_t)((buf[rb+8] << 8) | buf[rb+9]);
        uint16_t noff= (uint16_t)((buf[rb+10] << 8) | buf[rb+11]);
        if (nid != 1 && nid != 4 && nid != 6) continue;
        bool okPid = (pid == 0 || pid == 3 || pid == 1);
        if (!okPid) continue;
        uint32_t start = so + noff;
        if (start + len > buf.size()) continue;
        std::wstring s;
        if (pid == 0 || pid == 3) {
            if (len % 2) continue;
            s.resize(len / 2);
            for (uint16_t k = 0; k < len / 2; ++k)
                s[k] = (wchar_t)((buf[start + 2*k] << 8) | buf[start + 2*k + 1]);
        } else {
            s.resize(len);
// pid == 1 (Mac, ASCII/Latin)
// pid == 1 (Mac, ASCII/Latin)
            for (uint16_t k = 0; k < len; ++k) s[k] = (wchar_t)buf[start + k];
        }
        if (s.empty()) continue;
        if (nid == 1) { best = s; break; }
        if (best.empty()) best = s;
    }
// 族名优先
// family name takes priority
    return best;
}
bool GdiFontRasterizer::CreateFromFile(const wchar_t* filePath, int heightPx, int weight, bool italic,
                                       bool antiAlias) {
    if (m_hdc) return false;

#pragma endregion

#pragma region "Create / CreateFromFile"
    m_fontRes = AddFontResourceExW(filePath, FR_PRIVATE, 0);
    std::wstring face = ReadTtfFamilyName(filePath);
    if (face.empty()) {
    // FR_PRIVATE：仅本进程可见，不写注册表、不需管理员权限；卸载用 RemoveFontResourceEx。
    // FR_PRIVATE: visible to this process only, writes no registry entry, needs
    // no admin rights; unload via RemoveFontResourceEx.
        if (m_fontRes) { RemoveFontResourceExW(filePath, FR_PRIVATE, 0); m_fontRes = 0; }
        return false;
    }
        // 读不到族名：无法按文件建字体（文件损坏/非 TTF）
        // Could not read the family name: cannot build a font from the file
        // (file is corrupt / not a TTF).
    m_fontFile = filePath;
    bool ok = Create(face.c_str(), heightPx, weight, italic, antiAlias);
    if (!ok && m_fontRes) { RemoveFontResourceExW(filePath, FR_PRIVATE, 0); m_fontRes = 0; m_fontFile.clear(); }
    return ok;
}
bool GdiFontRasterizer::Create(const wchar_t* fontName, int heightPx, int weight, bool italic,
                               bool antiAlias) {
    if (m_hdc) return false;


    m_hdc = CreateCompatibleDC(nullptr);
    if (!m_hdc) return false;
    int quality = antiAlias ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY;
    m_hfont = CreateFontW(-heightPx, 0, 0, 0, weight, italic ? 1 : 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,

    // ★ 10:3x 抗锯齿开关：ANTIALIASED=灰度抗锯齿(平滑/发虚)；NONANTIALIASED=硬边(清晰/锯齿)

    // ★ switch for the 10:3x antialiasing option: ANTIALIASED = grayscale
    // antialiasing (smooth / faint); NONANTIALIASED = hard edges (crisp / jagged)
                          quality, DEFAULT_PITCH | FF_DONTCARE, fontName);
    if (!m_hfont) { DeleteDC(m_hdc); m_hdc = nullptr; return false; }
    SelectObject(m_hdc, m_hfont);
    m_height = heightPx;
    TEXTMETRICW tm = {};
    if (GetTextMetricsW(m_hdc, &tm)) { m_ascent = tm.tmAscent; m_descent = tm.tmDescent; }


    else { m_ascent = heightPx * 4 / 5; m_descent = heightPx / 5; }
    m_cellW = heightPx * 2 + 6;
    m_cellH = m_ascent + m_descent + 6;
    m_baseY = m_ascent + 3;

    // 内存 DIB（32bpp，自上而下）作为逐字形渲染画布。
    // 关键：用 TA_BASELINE 对齐，把 (penX, baseY) 当作"基线"落点；
    // 画布高度必须覆盖基线上方(ascent)与下方(descent)，否则字形会被裁掉。

    // In-memory DIB (32bpp, top-down) as the per-glyph render canvas.
    // Key point: align with TA_BASELINE, treating (penX, baseY) as the baseline
    // landing point; the canvas height must cover above-baseline (ascent) and
    // below-baseline (descent), otherwise the glyph gets clipped.
    BITMAPINFOHEADER bi = {};
// 宽：覆盖全宽 CJK(~height) + 余量
// width: covers full-width CJK (~height) + margin
    bi.biSize = sizeof(bi);
    bi.biWidth = m_cellW;
// 高：ascent+descent + 上下各 3 余量
// height: ascent+descent + 3 margin on each side
    bi.biHeight = -m_cellH;
    bi.biPlanes = 1;
// 基线在 DIB 中的 y（顶部留 3 余量）
// baseline y within the DIB (3px top margin)
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    m_hbmp = CreateDIBSection(m_hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, (void**)&m_bits, nullptr, 0);
    if (!m_hbmp) { DeleteDC(m_hdc); m_hdc = nullptr; DeleteObject(m_hfont); m_hfont = nullptr; return false; }
    SelectObject(m_hdc, m_hbmp);
// 负 = 自上而下
// negative = top-down
    SetTextAlign(m_hdc, TA_LEFT | TA_BASELINE);
    SetBkMode(m_hdc, OPAQUE);
    SetBkColor(m_hdc, RGB(255, 255, 255));
    SetTextColor(m_hdc, RGB(0, 0, 0));
    return true;
}
const Glyph* GdiFontRasterizer::GetGlyph(uint32_t codepoint) {
    auto it = m_cache.find(codepoint);
// 落点 (x,y) = 字形基线
// landing point (x,y) = glyph baseline
    if (it != m_cache.end()) return &it->second;
    Glyph g;
    if (!Rasterize(codepoint, g)) {
        g.w = g.h = g.pitch = g.originX = g.originY = g.advance = 0;
    }
    auto res = m_cache.emplace(codepoint, std::move(g));

#pragma endregion

#pragma region "GetGlyph / Rasterize"
    return &res.first->second;
}
bool GdiFontRasterizer::Rasterize(uint32_t cp, Glyph& out) {
    if (!m_hdc || !m_bits) return false;
    memset(m_bits, 0xFF, (size_t)m_cellW * m_cellH * 4);
    wchar_t wc = (wchar_t)cp;
    TextOutW(m_hdc, 3, m_baseY, &wc, 1);
    SIZE sz = {};
    GetTextExtentPoint32W(m_hdc, &wc, 1, &sz);
    int minX = m_cellW, minY = m_cellH, maxX = -1, maxY = -1;


    for (int y = 0; y < m_cellH; ++y) {
        const uint8_t* row = m_bits + (size_t)y * m_cellW * 4;
    // 清白底
    // clear to white background
        for (int x = 0; x < m_cellW; ++x) {
    // 画黑字
    // draw black glyph
            const uint8_t* p = row + x * 4;
            if (p[0] < 250 || p[1] < 250 || p[2] < 250) {
    // 量宽高（逻辑步进）
    // measure width/height (logical advance)
                if (x < minX) minX = x;
                if (y < minY) minY = y;

    // 扫描墨迹 bbox + 反算覆盖度

    // scan the ink bbox + derive coverage
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
            }
        }
    }
            // 接近白（覆盖<阈值）视为空白
            // near-white (coverage below threshold) counts as blank
    if (maxX < 0) {
        out.w = out.h = out.pitch = 0;
        out.originX = 0; out.originY = 0; out.advance = (int)sz.cx;
        return true;
    }
    int bw = maxX - minX + 1, bh = maxY - minY + 1;
    out.w = bw; out.h = bh; out.pitch = bw;
    out.originX = minX - 3;
    out.originY = minY - m_baseY;
        // 纯空白字形（空格类）：给合法 advance，零尺寸
        // purely blank glyph (space-like): give a valid advance, zero size
    out.advance = (int)sz.cx;
    out.alpha.resize((size_t)bw * bh);
    for (int y = 0; y < bh; ++y) {
        const uint8_t* src = m_bits + (size_t)(minY + y) * m_cellW * 4 + minX * 4;
        uint8_t* dst = out.alpha.data() + (size_t)y * bw;
        for (int x = 0; x < bw; ++x) {
            const uint8_t* p = src + x * 4;
// 相对 pen(3, baseY) 的偏移
// offset relative to pen (3, baseY)
            int lum = (p[2] * 77 + p[1] * 150 + p[0] * 29) / 256;
            dst[x] = (uint8_t)(255 - lum);
        }
    }
    return true;
}
std::vector<uint32_t> GdiFontRasterizer::DecodeUtf8(const char* s, size_t n) {
    std::vector<uint32_t> cps;
    cps.reserve(n);
            // 覆盖度 = 255 - 亮度（白=0，黑=255）
            // coverage = 255 - luminance (white=0, black=255)
    size_t i = 0;
// 近似 0.299/0.587/0.114
// approx 0.299/0.587/0.114
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int len;
        if (c < 0x80)       { cp = c;        len = 1; }
        else if ((c>>5)==6) { cp = c & 0x1F; len = 2; }
        else if ((c>>4)==0xE){ cp = c & 0x0F; len = 3; }

#pragma endregion

#pragma region "DecodeUtf8"
        else if ((c>>3)==0x1E){cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + (size_t)len > n) { ++i; continue; }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            unsigned char cc = (unsigned char)s[i+k];
            if ((cc>>6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok) cps.push_back(cp);
        i += (size_t)len;
    }
    return cps;
}
void GdiFontRasterizer::BlitGlyph(uint8_t* fb, int pitch, int bpp,
                                  int dx, int dy, const Glyph* g, uint32_t color,
                                  int fbW, int fbH, bool idempotent,
                                  int clipX, int clipY, int clipW, int clipH,
                                  int outlineWidth, uint32_t outlineColor) {
    if (!fb || !g || g->w == 0 || g->h == 0) return;
    if (fbW <= 0 || fbH <= 0) return;
    bool doClip = (clipW > 0 && clipH > 0);

    // ---- 单像素混合（带 idempotent / clip / 边界裁剪） ----
    // ---- per-pixel blend (idempotent / clip / boundary) ----
    auto BlitAt = [&](int ox, int oy, uint8_t tr, uint8_t tg, uint8_t tb) {
        for (int j = 0; j < g->h; ++j) {
            int fy = dy + oy + j;
            if (fy < 0 || fy >= fbH) continue;
            if (doClip && (fy < clipY || fy >= clipY + clipH)) continue;
            const uint8_t* srcRow = g->alpha.data() + (size_t)j * g->pitch;
            for (int i = 0; i < g->w; ++i) {
    // ★ 00:4x 引擎 UI 框裁剪（原行为 Rect::Intersect）：clipW/H > 0 时字形像素超出该矩形不画
    //   （修垂直列表最后一行溢出——原版/LG4 都按 clip 裁，我们之前只按表面边界裁 → 溢出可见）
    // ★ 00:4x engine UI-box clipping (original behavior Rect::Intersect): when
    // clipW/H > 0, glyph pixels outside that rectangle are not drawn
    // (fixes the last-row overflow in vertical lists -- original / LG4 both clip
    // by the box, but we previously only clipped by the surface boundary ->
    // overflow was visible).
                int fx = dx + ox + i;
                if (fx < 0 || fx >= fbW) continue;
                if (doClip && (fx < clipX || fx >= clipX + clipW)) continue;
                int a = srcRow[i];
                if (a == 0) continue;
                if (bpp == 1) {
                    // 8bpp palettized: write the palette index directly. Do NOT
                    // barycentric-blend old->tr in index space: the C1 palette is
                    // not a grayscale ramp, so interpolated indices map to
                    // arbitrary colors and produce ghosting / noisy edges.
                    uint8_t* p = fb + (size_t)fy * pitch + (size_t)fx;
                    if (idempotent && *p == tr) continue;  // already same color
                    *p = tr;
                } else if (bpp == 2) {
                    uint16_t* p = (uint16_t*)(fb + (size_t)fy * pitch + (size_t)fx * 2);
    // ★ 上下边界裁剪（防越界写）
    // ★ top/bottom boundary clip (prevent OOB write)
                    if (idempotent) {
                        uint8_t r, gg, b; From565(*p, r, gg, b);
    // ★ UI 框上下裁剪
    // ★ UI-box top/bottom clip
                        int dr = (int)r - tr, dg = (int)gg - tg, db = (int)b - tb;
                        if (dr > -4 && dr < 4 && dg > -5 && dg < 5 && db > -4 && db < 4) continue;
                    }
                    uint8_t r, gg, b; From565(*p, r, gg, b);
                    r = (uint8_t)(r + ((tr - r) * a) / 255);
    // ★ 左右边界裁剪（防越界）
    // ★ left/right boundary clip (prevent OOB write)
                    gg = (uint8_t)(gg + ((tg - gg) * a) / 255);
                    b = (uint8_t)(b + ((tb - b) * a) / 255);
    // ★ UI 框左右裁剪
    // ★ UI-box left/right clip
                    *p = To565((uint32_t)((r << 16) | (gg << 8) | b));
                } else if (bpp == 4) {
                    uint8_t* p = fb + (size_t)fy * pitch + (size_t)fx * 4;
                    if (idempotent) {
                        int dr = (int)p[2] - tr, dg = (int)p[1] - tg, db = (int)p[0] - tb;
                        if (dr > -24 && dr < 24 && dg > -24 && dg < 24 && db > -24 && db < 24) continue;
                        // RGB565 通道容差（5/6/5 位 ≈ 8 位值 24/32/24）
                        // RGB565 channel tolerance (5/6/5 bits ~= 8-bit values 24/32/24)
                    }
                    p[0] = (uint8_t)(p[0] + ((tb - p[0]) * a) / 255);
                    p[1] = (uint8_t)(p[1] + ((tg - p[1]) * a) / 255);
                    p[2] = (uint8_t)(p[2] + ((tr - p[2]) * a) / 255);
                    p[3] = 0xFF;
                }
            }
        }
    };

    // ---- 前景 pass：正常位置画文本色 ----
    // ---- foreground pass: draw text color at the exact position ----
    uint8_t tr = (uint8_t)((color >> 16) & 0xFF);
    uint8_t tg = (uint8_t)((color >> 8)  & 0xFF);
    uint8_t tb = (uint8_t)( color        & 0xFF);

    // ---- 描边 pass：先把字形在四邻域偏移用描边色画一遍，再用前景覆盖
    //      中央，形成围绕字形的轮廓（8bpp 用索引，16bpp 用 RGB）----
    // ---- outline pass: draw glyph at neighbouring offsets in the outline
    //      color first, then the core overwrites the centre => a frame around
    //      the glyph. 8bpp uses the index, 16bpp uses RGB ----
    if (outlineWidth > 0) {  // 黑色(值==0)是合法描边，不能按 "!=0" 过滤
        uint8_t ot, og, ob;
        if (bpp == 1) { ot = (uint8_t)((outlineColor >> 16) & 0xFF); og = 0; ob = 0; }
        else          { ot = (uint8_t)((outlineColor >> 16) & 0xFF);
                        og = (uint8_t)((outlineColor >> 8) & 0xFF);
                        ob = (uint8_t)( outlineColor        & 0xFF); }
        for (int oy = -outlineWidth; oy <= outlineWidth; ++oy)
            for (int ox = -outlineWidth; ox <= outlineWidth; ++ox) {
                if (ox == 0 && oy == 0) continue;
                BlitAt(ox, oy, ot, og, ob);
            }
    }
    BlitAt(0, 0, tr, tg, tb);
}

#pragma region "SaveRGBAAsBMP"
bool GdiFontRasterizer::SaveRGBAAsBMP(const char* path, int w, int h, const uint8_t* rgba) {
    int stride = (w * 4 + 3) & ~3;
    int filesize = 54 + stride * h;
                    // 32bpp 通道容差 24：抗锯齿中心墨迹≈纯前景色，重绘直接跳过（防累积）
                    // 32bpp channel tolerance 24: antialiased central ink is ~=
                    // pure foreground, redraw skips it directly (prevents accumulation)
    std::vector<uint8_t> hdr(54, 0);
    hdr[0]='B'; hdr[1]='M';
    *(uint32_t*)&hdr[2] = (uint32_t)filesize;
    *(uint32_t*)&hdr[10] = 54;
    *(uint32_t*)&hdr[14] = 40;
    *(int32_t*)&hdr[18] = w;
    *(int32_t*)&hdr[22] = h;
    *(uint16_t*)&hdr[26] = 1;
    *(uint16_t*)&hdr[28] = 32;
    *(uint32_t*)&hdr[34] = (uint32_t)(stride * h);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0) return false;
    if (!f) return false;
    fwrite(hdr.data(), 1, 54, f);
    std::vector<uint8_t> row(stride, 0);
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* src = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            row[(size_t)x*4+0] = src[(size_t)x*4+2];
            row[(size_t)x*4+1] = src[(size_t)x*4+1];
            row[(size_t)x*4+2] = src[(size_t)x*4+0];
            row[(size_t)x*4+3] = 0xFF;
// 正高度 = 自下而上(bottom-up)；像素按 y=h-1..0 写出，与 bottom-up 一致（纠正原先 -h 与写出顺序矛盾导致的上下颠倒）
// positive height = bottom-up; pixels are written in
        }
    // y = h-1..0 order, matching bottom-up (corrects the previous contradiction
    // between -h and the write order that caused vertical flipping)
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    return true;
}
}
}
#pragma endregion
