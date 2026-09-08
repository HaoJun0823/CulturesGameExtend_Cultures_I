// GdiFont.h
// ===================================================================
// GDI 字形光栅化引擎（多语言文本渲染重做的核心，游戏无关、可独立测试）
//
// 设计要点（对照 LG4Northland.dll + 你的决策 UTF-8 / 不用 FreeType / 游戏不跨平台）：
//   1. 输入是 UTF-8 字节流；先解码成 Unicode 码点（与磁盘文件编码彻底解耦）。
//   2. 每个码点用 Windows GDI 从任意已安装 TTF（繁中=微軟正黑體 / 細明體等）
//      动态光栅化成内存灰度位图 —— 这就是"从 TTF 动态生成到内存"，零外部
//      依赖（已链接 gdi32.lib）。
//      ★ 光栅化走 DIB + TextOutW 路线（不是 GetGlyphOutline）：无显示会话里
//        GetGlyphOutline 的灰度缓冲格式易崩溃，TextOutW 最稳、兼容最好。
//      ★ 必须用 SetTextAlign(TA_BASELINE) —— TextOut 默认 TA_TOP 会把 (x,y)
//        当成字形"顶边"，导致整字落在画布下方被裁掉（曾卡住数轮排查）。
//   3. 字形带缓存（码点 -> 灰度图），避免每帧重绘重复光栅化。
//   4. BlitGlyph 把灰度字形按文本色 alpha 混合进 32bpp(RGBA/RGB) 或 16bpp(RGB565)
//      目标缓冲，算法与 LG4 sub_10002D40 一致（编码无关，只认灰度+颜色）。
//
// 该模块不 hook 任何游戏函数；它只负责"码点 -> 内存里的字形位图"。
// 真正的游戏内接通（hook 文本绘制入口、把游戏字形源替换成这里）是另一个步骤。
// ===================================================================
// GdiFont.h
// ===================================================================
// GDI glyph rasterization engine (core of the multilingual text-rendering
// rework; game-agnostic and independently testable).
//
// Design notes (relative to LG4Northland.dll + your decisions: UTF-8 / no
// FreeType / game is not cross-platform):
//   1. Input is a UTF-8 byte stream; it is first decoded into Unicode
//      codepoints (fully decoupled from the on-disk file encoding).
//   2. For each codepoint, Windows GDI dynamically rasterizes it from any
//      installed TTF (Traditional Chinese = Microsoft JhengHei / MingLiU, etc.)
//      into an in-memory grayscale bitmap -- this is the "dynamically generate
//      from TTF into memory" approach, with zero external dependencies (gdi32.lib
//      is already linked).
//      ★ Rasterization uses the DIB + TextOutW path (not GetGlyphOutline): in a
//        session without a display, GetGlyphOutline's grayscale buffer format is
//        prone to crashes; TextOutW is the most stable and most compatible.
//      ★ SetTextAlign(TA_BASELINE) is mandatory -- TextOut's default TA_TOP
//        treats (x,y) as the glyph's "top edge", which pushes the whole glyph
//        below the canvas and gets it clipped (this once stalled several rounds
//        of debugging).
//   3. Glyphs are cached (codepoint -> grayscale bitmap) to avoid re-rasterizing
//      every frame.
//   4. BlitGlyph alpha-blends the grayscale glyph into a 32bpp (RGBA/RGB) or
//      16bpp (RGB565) destination buffer, using the same algorithm as LG4
//      sub_10002D40 (encoding-agnostic; only looks at grayscale + color).
//
// This module hooks no game functions; it is only responsible for
// "codepoint -> glyph bitmap in memory". The actual game integration (hooking
// the text-draw entry point, replacing the game's glyph source with this) is a
// separate step.
// ===================================================================
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>


namespace ge {
namespace text {

// 单个字形：灰度 alpha 位图（8 位每像素，0=透明，255=不透明）。
// 坐标约定同 GDI：y 向下；基线(baseline)是文字落地的水平线。

#pragma region "Glyph"
// A single glyph: a grayscale alpha bitmap (8 bits per pixel, 0 = transparent,
// 255 = opaque). Coordinate convention matches GDI: y grows downward; the
// baseline is the horizontal line the text rests on.
struct Glyph {
    int w = 0;
// 黑框宽度  gmBlackBoxX
// black-box width  gmBlackBoxX
    int h = 0;
    int pitch = 0;
// 黑框高度  gmBlackBoxY
// black-box height gmBlackBoxY
    int originX = 0;
    int originY = 0;
// 每行字节数（4 字节对齐，GGO_GRAY8 规定）
// bytes per row (4-byte aligned, per GGO_GRAY8)
    int advance = 0;
    std::vector<uint8_t> alpha;
// 黑框左上角相对"笔位(pen)"的 x 偏移 gmptGlyphOrigin.x
// x offset of the black-box top-left relative to the pen (gmptGlyphOrigin.x)
};
class GdiFontRasterizer {
// 黑框左上角相对"笔位"的 y 偏移 gmptGlyphOrigin.y（通常为负，向上）
// y offset of the black-box top-left relative to the pen (gmptGlyphOrigin.y, usually negative, upward)
public:
    GdiFontRasterizer() = default;
// 光标水平步进 gmCellIncX
// horizontal cursor advance gmCellIncX
    ~GdiFontRasterizer();
    GdiFontRasterizer(const GdiFontRasterizer&) = delete;
// 灰度数据，长度 = pitch * h
// grayscale data, length = pitch * h
    GdiFontRasterizer& operator=(const GdiFontRasterizer&) = delete;
    bool Create(const wchar_t* fontName, int heightPx, int weight = 400, bool italic = false,

#pragma endregion

#pragma region "GdiFontRasterizer (public API)"
                bool antiAlias = true);
    bool CreateFromFile(const wchar_t* filePath, int heightPx, int weight = 400, bool italic = false,
                        bool antiAlias = true);
    void Reset();


    const Glyph* GetGlyph(uint32_t codepoint);
    int Height()  const { return m_height; }

    // 用指定字体名 / 像素高度创建光栅器。失败返回 false。
    // fontName 例：L"Microsoft JhengHei"(繁中) / L"Microsoft YaHei"(简中) / L"SimSun"
    // antiAlias: true=灰度抗锯齿(平滑但可能发虚) / false=硬边(NONANTIALIASED，清晰但锯齿)。

    // Create the rasterizer with the given font name / pixel height. Returns
    // false on failure.
    // fontName examples: L"Microsoft JhengHei" (Trad. Chinese) / L"Microsoft YaHei"
    // (Simp. Chinese) / L"SimSun".
    // antiAlias: true = grayscale antialiasing (smooth but may look faint) /
    // false = hard edges (NONANTIALIASED, crisp but jagged).
    int Ascent()  const { return m_ascent; }
    int Descent() const { return m_descent; }

    // 从 .ttf 文件加载字体（自包含，不依赖系统是否安装该字体）。
    // 内部用 AddFontResourceEx(FR_PRIVATE) 把文件注册到本进程，再按文件内的
    // 族名创建。filePath 例：L"plugins/fonts/l10.ttf"。

    // Load a font from a .ttf file (self-contained; does not depend on whether
    // the font is installed system-wide). Internally registers the file into
    // this process via AddFontResourceEx(FR_PRIVATE), then creates it by the
    // family name found inside the file. filePath example: L"plugins/fonts/l10.ttf".
    static std::vector<uint32_t> DecodeUtf8(const char* s, size_t n);
    static void BlitGlyph(uint8_t* fb, int pitch, int bpp,

                          // 释放参数：将当前字体与画布释放，回到初始状态（便于按游戏语言热切换字体）。

                          // Release the current font and canvas, returning to the initial state (useful
                          // for hot-swapping the font when the game language changes).
                          int dx, int dy, const Glyph* g, uint32_t color,

                          // 取字形（带缓存）。取不到返回 nullptr（调用方用空格/豆腐占位）。

                          // Get a glyph for a codepoint (cached). Returns nullptr if unavailable
                          // (caller should substitute a space / tofu box).
                          int fbW, int fbH, bool idempotent = true,

                          int clipX = -1, int clipY = -1, int clipW = -1, int clipH = -1);
    static bool SaveRGBAAsBMP(const char* path, int w, int h, const uint8_t* rgba);
private:

    // ---------- 纯工具（静态，便于独立测试）----------

    // UTF-8 字节流 -> Unicode 码点序列（符合 RFC 3629，越界字节跳过）。

    // ---------- pure utilities (static, for easy independent testing) ----------

    // UTF-8 byte stream -> Unicode codepoint sequence (RFC 3629 compliant;
    // out-of-range bytes are skipped).
    bool Rasterize(uint32_t cp, Glyph& out);

    // 把灰度字形按文本色混合进帧缓冲。
    //   fb    : 目标缓冲首字节
    //   pitch : 每行字节数
    //   bpp   : 2 = 16bpp RGB565；4 = 32bpp（内存布局 B,G,R[,A]）
    //   dx,dy : 字形黑框左上角在 fb 中的像素位置（调用方应已加 originX/originY 校正）
    //   color : 0xRRGGBB（16bpp 时自动转 RGB565）
    //   fbW,fbH : 目标表面像素宽/高（用于边界裁剪，防止写出缓冲外导致崩溃）
    //   idempotent : 像素级幂等（1=推荐）：目标像素已≈前景色则跳过该像素，避免
    //     不清空表面上的跨帧重绘累积（LG4 tga 位图不透明直写无累积，GDI 抗锯齿
    //     alpha 混合会累积变浓；幂等 = 保留抗锯齿 + 同位置重绘像素不变）。
    //   clipX/Y/W/H : 引擎 UI 框裁剪区（DrawContext +0x08..0x14）。W/H > 0 时启用，
    //     字形像素超出该矩形的不画（引擎原行为 Rect::Intersect，修垂直列表最后一行溢出）。
#pragma endregion

#pragma region "GdiFontRasterizer (blit / save)"
    // Blend a grayscale glyph into the framebuffer using the text color.
    //   fb    : first byte of the destination buffer
    //   pitch : bytes per row
    //   bpp   : 2 = 16bpp RGB565; 4 = 32bpp (memory layout B,G,R[,A])
    //   dx,dy : pixel position of the glyph black-box top-left within fb
    //           (caller should already have applied the originX/originY correction)
    //   color : 0xRRGGBB (auto-converted to RGB565 when bpp == 2)
    //   fbW,fbH : destination surface pixel width/height (used for boundary
    //           clipping, to prevent out-of-buffer writes that crash)
    //   idempotent : pixel-level idempotency (1 = recommended): if the target
    //           pixel is already ~= the foreground color, skip that pixel, to
    //           avoid cross-frame redraw accumulation on a surface that isn't
    //           cleared (LG4 tga bitmaps write opaque with no accumulation; GDI
    //           antialiasing alpha blending accumulates and darkens; idempotent =
    //           keep antialiasing + unchanged pixels on redraw at the same spot).
    //   clipX/Y/W/H : engine UI-box clip region (DrawContext +0x08..0x14). Enabled
    //           when W/H > 0; glyph pixels outside this rectangle are not drawn
    //           (matches the engine's original Rect::Intersect behavior; fixes the
    //           last-row overflow in vertical lists).
    HDC   m_hdc   = nullptr;
    HFONT m_hfont = nullptr;
    HBITMAP m_hbmp = nullptr;
    uint8_t* m_bits = nullptr;

    // 把 RGBA 缓冲（白底文字）保存为 32bpp BMP（用于独立验证 / 自测落盘）。

    // Save an RGBA buffer (white-background text) as a 32bpp BMP (for standalone
    // verification / self-test dumping).
    int    m_fontRes = 0;

#pragma endregion

#pragma region "GdiFontRasterizer (private)"
    std::wstring m_fontFile;
    static std::wstring ReadTtfFamilyName(const wchar_t* path);


    int   m_height = 0;
    int   m_ascent = 0;
    int   m_descent = 0;
// 逐字形渲染画布（内存 DIB）
// per-glyph render canvas (in-memory DIB)
    int   m_cellW = 0, m_cellH = 0, m_baseY = 0;
    std::unordered_map<uint32_t, Glyph> m_cache;
// DIB 像素指针
// DIB pixel pointer
};
}
// AddFontResourceEx 返回值（新增字体数，>0 表示已注册）
// return value of AddFontResourceEx (number of fonts added; >0 means registered)
}
#pragma endregion
