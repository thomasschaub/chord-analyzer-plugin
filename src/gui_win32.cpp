// Win32 editor. Everything except the two scale combo boxes is drawn with
// Direct2D/DirectWrite into a single child window; DirectWrite's font
// fallback covers glyphs Segoe UI lacks (e.g. ♭).

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gui_api.h"
#include "midi_file.hpp"
#include "scale.hpp"

namespace {

namespace fs = std::filesystem;

constexpr float kWidth  = 440;
constexpr float kHeight = 520;

constexpr int kRangeStart = 36; // C2
constexpr int kRangeEnd   = 83; // B5

constexpr UINT_PTR kPollTimer   = 1;
constexpr UINT     kPollMs      = 60;
constexpr int      kRootComboId = 101;
constexpr int      kTypeComboId = 102;
// Tooltip ids: pads use 1..N, the buttons these.
constexpr UINT_PTR kUseTipId    = 200;
constexpr UINT_PTR kResetTipId  = 201;

inline bool isWhitePc(int pc) {
    switch (pc) {
        case 0: case 2: case 4: case 5: case 7: case 9: case 11: return true;
        default: return false;
    }
}

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

constexpr D2D1_COLOR_F rgb(float r, float g, float b) { return {r, g, b, 1.0f}; }
constexpr D2D1_COLOR_F gray(float w) { return {w, w, w, 1.0f}; }

bool sameColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

constexpr D2D1_COLOR_F kBackground = rgb(0.09f, 0.09f, 0.11f);
constexpr D2D1_COLOR_F kDimText    = gray(0.55f);
constexpr D2D1_COLOR_F kWhite      = gray(1.0f);

D2D1_RECT_F rectXYWH(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
D2D1_RECT_F insetRect(D2D1_RECT_F r, float d) { return {r.left + d, r.top + d, r.right - d, r.bottom - d}; }
bool contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

template <class T> void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Shared per process; created with the first view, released with the last.
ID2D1Factory*   gD2D       = nullptr;
IDWriteFactory* gDWrite    = nullptr;
int             gViewCount = 0;
wchar_t         gClassName[64] = {};

HINSTANCE moduleHandle() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleHandle), &m);
    return m;
}

IDWriteTextFormat* makeFormat(float size, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextFormat* f = nullptr;
    gDWrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return f;
}

float textWidth(IDWriteTextFormat* f, const std::wstring& s) {
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(gDWrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 100000, 1000, &layout))) {
        return 0;
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return m.widthIncludingTrailingWhitespace;
}

// ------------------------------------------------------------
// View state
// ------------------------------------------------------------

struct Pad {
    int degree = 0;
    int variant = 0; // scale::ChordVariant; non-triad pads are compact
    std::wstring numeral, name, tip;
    bool enabled = false;
    D2D1_RECT_F r{};
};

struct Button {
    std::wstring title, tip;
    bool enabled = false;
    D2D1_RECT_F r{};
};

struct View {
    ChordGuiCallbacks cb{};
    double scale = 1.0;
    bool oleInitialized = false;

    HWND hwnd = nullptr;
    HWND rootCombo = nullptr;
    HWND typeCombo = nullptr;
    HWND tooltip = nullptr;
    HBRUSH listBrush = nullptr;
    WNDPROC comboBaseProc = nullptr; // both combos share the system class

    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush*  brush = nullptr;
    // Combo items are owner-drawn through a DC target so they get the same
    // text rendering (and font fallback) as the rest of the view.
    ID2D1DCRenderTarget*   dcRt = nullptr;
    ID2D1SolidColorBrush*  dcBrush = nullptr;

    IDWriteTextFormat* fLabel = nullptr;    // 12 medium
    IDWriteTextFormat* fOctave = nullptr;   // 8 medium
    IDWriteTextFormat* fHist = nullptr;     // 10 medium
    IDWriteTextFormat* fNumeral = nullptr;  // 15 semibold
    IDWriteTextFormat* fPadName = nullptr;  // 10 medium
    IDWriteTextFormat* fVariant = nullptr;  // 9 medium
    IDWriteTextFormat* fDetected = nullptr; // 13 semibold
    IDWriteTextFormat* fButton = nullptr;   // 11 regular
    IDWriteTextFormat* fChord = nullptr;    // sized to fit the chord name
    IDWriteTextFormat* fCombo = nullptr;    // pixel-sized for the DC target
    float chordFontSize = 0;

    // Layout, in logical units.
    D2D1_RECT_F subtitleR{}, chordR{}, keyboardR{}, scaleLabelR{}, rootComboR{}, typeComboR{};
    D2D1_RECT_F detectedTitleR{}, detectedNameR{}, histR{};
    Button useButton, resetButton;
    std::vector<Pad> pads;

    // Displayed state.
    std::wstring chordText = L"—";
    D2D1_COLOR_F chordColor = kWhite;
    std::bitset<128> held;
    bool chordRecognized = false;
    uint16_t kbMask = 0;
    int kbRoot = 0;
    uint16_t histCounts[12] = {};
    uint16_t histMask = 0;
    int histRoot = 0;
    int curRoot = 0, curType = 0;
    int padsRoot = -1, padsType = -1; // scale the pads are labelled for
    int detectedRoot = 0, detectedType = 0;
    std::wstring detectedText;
    D2D1_COLOR_F detectedColor = kDimText;

    // Mouse interaction.
    int pressedPad = -1;
    bool dragStarted = false;
    POINT downPt{};
    Button* pressedButton = nullptr;
};

float px(const View* v, float units) { return units * (float)v->scale; }
// Rounds a logical coordinate to the nearest physical pixel.
float snap(const View* v, float units) { return std::round(px(v, units)) / (float)v->scale; }

RECT toPixels(const View* v, const D2D1_RECT_F& r) {
    return {std::lround(px(v, r.left)), std::lround(px(v, r.top)),
            std::lround(px(v, r.right)), std::lround(px(v, r.bottom))};
}

void invalidate(View* v) {
    if (v->hwnd) InvalidateRect(v->hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------
// Layout
// ------------------------------------------------------------

void layout(View* v) {
    const float W = kWidth, H = kHeight;
    const float pad = 10;
    const float kbH = 92;
    const float rowH = 22;
    const float rowGap = 8;
    const float histH = 80;
    const float sectionGap = 14;
    const float padH = 44;
    const float variantH = 16;
    const float variantGap = 2;
    const float variantBlockH = 4 + variantH * 2 + variantGap;

    v->subtitleR = rectXYWH(0, pad, W, 16);

    // Bottom-up: histogram, detection row, keyboard, pads, scale row.
    float histY = H - pad - histH;
    float detectY = histY - 6 - rowH;
    float kbY = detectY - sectionGap - kbH;
    float padY = kbY - rowGap - variantBlockH - padH;
    float rowY = padY - rowGap - rowH;

    float chordY = pad + 22;
    float chordH = std::max(40.0f, rowY - pad - chordY);
    v->chordR = rectXYWH(12, chordY, W - 24, chordH);

    float right = W - pad;
    v->resetButton.r = rectXYWH(right - 60, detectY, 60, rowH);
    v->useButton.r = rectXYWH(right - 60 - 4 - 50, detectY, 50, rowH);
    v->detectedTitleR = rectXYWH(pad, detectY + 3, 62, 16);
    v->detectedNameR = rectXYWH(pad + 62, detectY + 2,
                                v->useButton.r.left - 6 - pad - 62, 18);
    v->histR = rectXYWH(pad, histY, W - pad * 2, histH);
    v->scaleLabelR = rectXYWH(pad, rowY + 3, 40, 16);
    v->rootComboR = rectXYWH(pad + 40, rowY, 84, rowH);
    v->typeComboR = rectXYWH(pad + 40 + 84 + 6, rowY, 140, rowH);

    float padGap = 6;
    float padW = (W - pad * 2 - padGap * 6) / 7.0f;
    // Variant buttons sit in a 2x2 grid under their pad: sus2 sus4 / 7 9.
    float variantW = std::floor((std::round(padW) - variantGap) * 0.5f);
    for (Pad& p : v->pads) {
        float x = std::round(pad + p.degree * (padW + padGap));
        if (p.variant == (int)scale::ChordVariant::Triad) {
            p.r = rectXYWH(x, padY, std::round(padW), padH);
            continue;
        }
        int cell = p.variant - 1;
        float cx = x + (cell % 2) * (variantW + variantGap);
        float cy = padY + padH + 4 + (cell / 2) * (variantH + variantGap);
        p.r = rectXYWH(cx, cy, variantW, variantH);
    }

    v->keyboardR = rectXYWH(pad, kbY, W - pad * 2, kbH);
}

// ------------------------------------------------------------
// Drawing
// ------------------------------------------------------------

void fill(View* v, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    v->brush->SetColor(&c);
    v->rt->FillRectangle(&r, v->brush);
}

void fillRound(View* v, const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
    D2D1_ROUNDED_RECT rr{r, radius, radius};
    v->brush->SetColor(&c);
    v->rt->FillRoundedRectangle(&rr, v->brush);
}

void strokeRound(View* v, const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
    D2D1_ROUNDED_RECT rr{r, radius, radius};
    v->brush->SetColor(&c);
    v->rt->DrawRoundedRectangle(&rr, v->brush, 1.0f, nullptr);
}

// One physical pixel wide outline just inside r.
void frame(View* v, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    float p = 1.0f / (float)v->scale;
    fill(v, {r.left, r.top, r.right, r.top + p}, c);
    fill(v, {r.left, r.bottom - p, r.right, r.bottom}, c);
    fill(v, {r.left, r.top, r.left + p, r.bottom}, c);
    fill(v, {r.right - p, r.top, r.right, r.bottom}, c);
}

void drawText(View* v, const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r,
              const D2D1_COLOR_F& c,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER) {
    if (!f || s.empty()) return;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    v->brush->SetColor(&c);
    v->rt->DrawText(s.c_str(), (UINT32)s.size(), f, &r, v->brush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void drawRootMarker(View* v, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    float d = std::min((r.right - r.left) * 0.32f, 6.0f);
    D2D1_ELLIPSE e{{(r.left + r.right) * 0.5f, r.bottom - d * 0.5f - 5}, d * 0.5f, d * 0.5f};
    v->brush->SetColor(&c);
    v->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    v->rt->FillEllipse(&e, v->brush);
    v->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
}

void drawKeyboard(View* v) {
    const D2D1_RECT_F b = v->keyboardR;
    const float W = b.right - b.left, H = b.bottom - b.top;

    int whiteCount = 0;
    for (int n = kRangeStart; n <= kRangeEnd; ++n) {
        if (isWhitePc(n % 12)) whiteCount++;
    }
    const float whiteW = W / (float)whiteCount;
    const float blackW = whiteW * 0.62f;
    const float blackH = H * 0.62f;

    const D2D1_COLOR_F whiteFill    = rgb(0.93f, 0.93f, 0.94f);
    const D2D1_COLOR_F blackFill    = rgb(0.09f, 0.09f, 0.12f);
    const D2D1_COLOR_F separator    = gray(0.35f);
    const D2D1_COLOR_F octaveMark   = gray(0.55f);
    const D2D1_COLOR_F blackOutline = rgb(0.05f, 0.05f, 0.07f);

    // Soft green tint for keys that belong to the selected scale.
    const D2D1_COLOR_F whiteScale = rgb(0.72f, 0.90f, 0.78f);
    const D2D1_COLOR_F blackScale = rgb(0.18f, 0.42f, 0.30f);
    // The scale root gets a stronger green plus a dot marker.
    const D2D1_COLOR_F whiteRoot = rgb(0.36f, 0.78f, 0.52f);
    const D2D1_COLOR_F blackRoot = rgb(0.42f, 0.93f, 0.60f);
    const D2D1_COLOR_F rootMark  = rgb(0.04f, 0.24f, 0.13f);

    // Blue when a chord is recognized, amber when it's just individual notes.
    const D2D1_COLOR_F whiteHeld = v->chordRecognized ? rgb(0.32f, 0.74f, 1.00f)
                                                      : rgb(1.00f, 0.72f, 0.24f);
    const D2D1_COLOR_F blackHeld = v->chordRecognized ? rgb(0.20f, 0.55f, 0.85f)
                                                      : rgb(0.75f, 0.52f, 0.14f);

    auto inScale = [v](int pc) { return ((v->kbMask >> pc) & 1) != 0; };

    // Keys are pixel-aligned rectangles; aliased keeps their edges crisp.
    v->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    // White keys pass
    int wi = 0;
    for (int n = kRangeStart; n <= kRangeEnd; ++n) {
        int pc = n % 12;
        if (!isWhitePc(pc)) continue;

        D2D1_RECT_F r{snap(v, b.left + wi * whiteW), b.top,
                      snap(v, b.left + (wi + 1) * whiteW), b.bottom};
        bool held = v->held[n];
        bool scaleKey = inScale(pc);
        bool isRoot = scaleKey && pc == v->kbRoot;
        fill(v, r, held ? whiteHeld : isRoot ? whiteRoot : scaleKey ? whiteScale : whiteFill);
        if (isRoot) {
            // Sit the marker above the octave label so they don't collide.
            D2D1_RECT_F markerArea = r;
            if (pc == 0) markerArea.bottom -= 12;
            drawRootMarker(v, markerArea, rootMark);
        }

        // Subtle "C" marker under each C key
        if (pc == 0) {
            std::wstring label = L"C" + std::to_wstring(n / 12 - 1);
            drawText(v, label, v->fOctave, {r.left, r.bottom - 3 - 12, r.right, r.bottom - 3},
                     octaveMark);
        }
        wi++;
    }

    // Thin separators between white keys, one physical pixel wide
    for (int i = 1; i < whiteCount; ++i) {
        float x = snap(v, b.left + i * whiteW);
        fill(v, {x, b.top, x + 1.0f / (float)v->scale, b.bottom}, separator);
    }

    // Black keys pass (drawn on top)
    wi = 0;
    for (int n = kRangeStart; n <= kRangeEnd; ++n) {
        int pc = n % 12;
        if (isWhitePc(pc)) {
            wi++;
            continue;
        }

        float x = snap(v, b.left + wi * whiteW - blackW * 0.5f);
        D2D1_RECT_F r{x, b.top, x + snap(v, blackW), b.top + snap(v, blackH)};
        bool held = v->held[n];
        bool scaleKey = inScale(pc);
        bool isRoot = scaleKey && pc == v->kbRoot;
        fill(v, r, held ? blackHeld : isRoot ? blackRoot : scaleKey ? blackScale : blackFill);

        // Dark contour so a highlighted black key doesn't blend into
        // highlighted white keys next to it.
        frame(v, r, blackOutline);

        if (isRoot) drawRootMarker(v, r, rootMark);
    }

    v->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void drawHistogram(View* v) {
    const D2D1_RECT_F b = v->histR;
    const float W = b.right - b.left, H = b.bottom - b.top;

    const D2D1_COLOR_F panel    = rgb(0.13f, 0.13f, 0.16f);
    const D2D1_COLOR_F barPlain = gray(0.42f);
    const D2D1_COLOR_F barScale = rgb(0.72f, 0.90f, 0.78f);
    const D2D1_COLOR_F barRoot  = rgb(0.42f, 0.93f, 0.60f);
    const D2D1_COLOR_F labelDim = gray(0.50f);
    const D2D1_COLOR_F labelLit = gray(0.85f);

    fillRound(v, b, 6, panel);

    uint16_t maxCount = 0;
    for (uint16_t c : v->histCounts) maxCount = std::max(maxCount, c);

    const float inset = 8;
    const float labelH = 14;
    const float colW = (W - inset * 2) / 12.0f;
    const float barW = std::round(colW * 0.62f);
    const float baseY = b.top + H - inset - labelH;
    const float maxBarH = baseY - (b.top + inset);

    for (int pc = 0; pc < 12; ++pc) {
        bool scaleKey = (v->histMask >> pc) & 1;
        bool isRoot = scaleKey && pc == v->histRoot;
        float colX = b.left + inset + pc * colW;

        if (maxCount > 0 && v->histCounts[pc] > 0) {
            float h = std::max(2.0f, std::round(maxBarH * v->histCounts[pc] / maxCount));
            float x = std::round(colX + (colW - barW) * 0.5f);
            fillRound(v, {x, baseY - h, x + barW, baseY}, 2,
                      isRoot ? barRoot : scaleKey ? barScale : barPlain);
        }

        drawText(v, widen(scale::kShortNames[pc]), v->fHist,
                 {colX, baseY + 1, colX + colW, baseY + 1 + labelH},
                 scaleKey ? labelLit : labelDim);
    }
}

void drawPad(View* v, const Pad& p, bool pressed) {
    D2D1_COLOR_F fillC, border, numeralColor, nameColor;
    if (!p.enabled) {
        fillC        = rgb(0.12f, 0.12f, 0.14f);
        border       = gray(0.20f);
        numeralColor = gray(0.32f);
        nameColor    = numeralColor;
    } else if (pressed) {
        fillC        = rgb(0.32f, 0.74f, 1.00f);
        border       = rgb(0.60f, 0.85f, 1.00f);
        numeralColor = rgb(0.05f, 0.10f, 0.16f);
        nameColor    = numeralColor;
    } else {
        fillC        = rgb(0.16f, 0.16f, 0.20f);
        border       = gray(0.30f);
        numeralColor = kWhite;
        nameColor    = gray(0.62f);
    }

    bool compact = p.variant != (int)scale::ChordVariant::Triad;
    float radius = compact ? 3 : 5;
    D2D1_RECT_F shape = insetRect(p.r, 0.5f);
    fillRound(v, shape, radius, fillC);
    strokeRound(v, shape, radius, border);

    if (compact) {
        // Variant buttons show only their label ("sus2", "7", ...).
        drawText(v, p.numeral, v->fVariant, p.r,
                 p.enabled && !pressed ? nameColor : numeralColor);
        return;
    }

    const float numeralH = 20, nameH = 13;
    float top = p.r.top + ((p.r.bottom - p.r.top) - numeralH - nameH) * 0.5f;
    drawText(v, p.numeral, v->fNumeral, {p.r.left, top, p.r.right, top + numeralH}, numeralColor);
    drawText(v, p.name, v->fPadName, {p.r.left, top + numeralH, p.r.right, top + numeralH + nameH},
             nameColor);
}

void drawButton(View* v, const Button& btn) {
    bool pressed = v->pressedButton == &btn;
    D2D1_COLOR_F fillC = !btn.enabled ? rgb(0.13f, 0.13f, 0.16f)
                       : pressed      ? rgb(0.30f, 0.30f, 0.36f)
                                      : rgb(0.20f, 0.20f, 0.24f);
    D2D1_RECT_F shape = insetRect(btn.r, 0.5f);
    fillRound(v, shape, 4, fillC);
    strokeRound(v, shape, 4, btn.enabled ? gray(0.36f) : gray(0.22f));
    drawText(v, btn.title, v->fButton, btn.r, btn.enabled ? gray(0.92f) : gray(0.40f));
}

void discardTarget(View* v) {
    release(v->brush);
    release(v->rt);
}

bool ensureTarget(View* v) {
    if (v->rt) return true;
    RECT rc;
    GetClientRect(v->hwnd, &rc);
    float dpi = 96.0f * (float)v->scale;
    D2D1_RENDER_TARGET_PROPERTIES props{};
    props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    props.pixelFormat = {DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN};
    props.dpiX = dpi;
    props.dpiY = dpi;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps{};
    hwndProps.hwnd = v->hwnd;
    hwndProps.pixelSize = {(UINT32)rc.right, (UINT32)rc.bottom};
    hwndProps.presentOptions = D2D1_PRESENT_OPTIONS_NONE;
    if (FAILED(gD2D->CreateHwndRenderTarget(&props, &hwndProps, &v->rt))) return false;
    v->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(v->rt->CreateSolidColorBrush(&kWhite, nullptr, &v->brush))) {
        discardTarget(v);
        return false;
    }
    return true;
}

void paint(View* v) {
    if (!ensureTarget(v)) return;
    v->rt->BeginDraw();
    v->rt->Clear(&kBackground);

    drawText(v, L"Chord", v->fLabel, v->subtitleR, kDimText);
    drawText(v, v->chordText, v->fChord, v->chordR, v->chordColor);
    drawText(v, L"Scale", v->fLabel, v->scaleLabelR, kDimText, DWRITE_TEXT_ALIGNMENT_LEADING);

    for (size_t i = 0; i < v->pads.size(); ++i) drawPad(v, v->pads[i], (int)i == v->pressedPad);

    drawKeyboard(v);

    drawText(v, L"Detected", v->fLabel, v->detectedTitleR, kDimText, DWRITE_TEXT_ALIGNMENT_LEADING);
    drawText(v, v->detectedText, v->fDetected, v->detectedNameR, v->detectedColor,
             DWRITE_TEXT_ALIGNMENT_LEADING);
    drawButton(v, v->useButton);
    drawButton(v, v->resetButton);

    drawHistogram(v);

    if (v->rt->EndDraw(nullptr, nullptr) == D2DERR_RECREATE_TARGET) discardTarget(v);
}

// ------------------------------------------------------------
// Combo boxes
// ------------------------------------------------------------

std::wstring comboItemText(int ctlId, int index) {
    if (index < 0) return {};
    if (ctlId == kRootComboId) return index < 12 ? widen(scale::kRootNames[index]) : L"";
    return index < scale::kTypeCount ? widen(scale::kTypes[index].name) : L"";
}

int comboFieldHeight(const View* v) { return std::max(8L, std::lround(px(v, 22)) - 6); }
int comboItemHeight(const View* v) { return (int)std::lround(px(v, 18)); }

const D2D1_COLOR_F kComboFill   = rgb(0.17f, 0.17f, 0.21f);
const D2D1_COLOR_F kComboListBg = rgb(0.13f, 0.13f, 0.16f);

bool ensureDcTarget(View* v) {
    if (v->dcRt) return true;
    D2D1_RENDER_TARGET_PROPERTIES props{};
    props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
    // Item rects arrive in pixels; draw 1:1 and size the font in pixels.
    props.dpiX = 96;
    props.dpiY = 96;
    if (FAILED(gD2D->CreateDCRenderTarget(&props, &v->dcRt))) return false;
    v->dcRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(v->dcRt->CreateSolidColorBrush(&kWhite, nullptr, &v->dcBrush))) {
        release(v->dcRt);
        return false;
    }
    return true;
}

void drawComboItem(View* v, const DRAWITEMSTRUCT* dis) {
    if (!ensureDcTarget(v) || !v->fCombo) return;
    RECT rc = dis->rcItem;
    if (FAILED(v->dcRt->BindDC(dis->hDC, &rc))) return;

    bool field = (dis->itemState & ODS_COMBOBOXEDIT) != 0;
    bool selected = !field && (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    D2D1_COLOR_F bg = selected ? rgb(0.20f, 0.45f, 0.75f) : field ? kComboFill : kComboListBg;
    D2D1_COLOR_F fg = disabled ? gray(0.42f) : gray(0.92f);

    float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
    D2D1_RECT_F all{0, 0, w, h};
    // rcItem is in combo client coordinates; keep the field text exactly
    // where paintComboField puts it.
    float textLeft = field ? px(v, 7) - (float)rc.left : px(v, 6);
    D2D1_RECT_F textR{textLeft, 0, w - px(v, 2), h};
    std::wstring text = comboItemText((int)dis->CtlID, (int)dis->itemID);

    v->dcRt->BeginDraw();
    v->dcBrush->SetColor(&bg);
    v->dcRt->FillRectangle(&all, v->dcBrush);
    if (!text.empty()) {
        v->fCombo->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        v->fCombo->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        v->dcBrush->SetColor(&fg);
        v->dcRt->DrawText(text.c_str(), (UINT32)text.size(), v->fCombo, &textR, v->dcBrush,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }
    if (v->dcRt->EndDraw(nullptr, nullptr) == D2DERR_RECREATE_TARGET) {
        release(v->dcBrush);
        release(v->dcRt);
    }
}

// The closed combo is painted here in full (frame, text, chevron) so it
// matches the pads; the system theme would draw a light frame and button.
// When the combo redraws just its field outside WM_PAINT, WM_DRAWITEM
// paints the same fill, so the two always agree.
void paintComboField(View* v, HWND combo, HDC dc) {
    if (!ensureDcTarget(v) || !v->fCombo) return;
    RECT rc;
    GetClientRect(combo, &rc);
    if (FAILED(v->dcRt->BindDC(dc, &rc))) return;

    bool enabled = IsWindowEnabled(combo) != FALSE;
    bool open = SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0;
    float w = (float)rc.right, h = (float)rc.bottom;
    float radius = px(v, 4);
    D2D1_ROUNDED_RECT shape{{0.5f, 0.5f, w - 0.5f, h - 0.5f}, radius, radius};
    D2D1_COLOR_F fillC = open ? rgb(0.22f, 0.22f, 0.27f) : kComboFill;
    D2D1_COLOR_F border = enabled ? gray(0.36f) : gray(0.22f);
    D2D1_COLOR_F fg = enabled ? gray(0.92f) : gray(0.42f);
    std::wstring text = comboItemText(GetDlgCtrlID(combo),
                                      (int)SendMessageW(combo, CB_GETCURSEL, 0, 0));
    float arrowW = px(v, 18);
    D2D1_RECT_F textR{px(v, 7), 0, w - arrowW, h};

    v->dcRt->BeginDraw();
    v->dcRt->Clear(&kBackground);
    v->dcBrush->SetColor(&fillC);
    v->dcRt->FillRoundedRectangle(&shape, v->dcBrush);
    v->dcBrush->SetColor(&border);
    v->dcRt->DrawRoundedRectangle(&shape, v->dcBrush, 1.0f, nullptr);
    v->dcBrush->SetColor(&fg);
    if (!text.empty()) {
        v->fCombo->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        v->fCombo->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        v->dcRt->DrawText(text.c_str(), (UINT32)text.size(), v->fCombo, &textR, v->dcBrush,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }
    // Chevron
    float cx = w - arrowW * 0.5f - px(v, 1), cy = h * 0.5f, a = px(v, 3.5f);
    float stroke = std::max(1.0f, px(v, 1.4f));
    v->dcRt->DrawLine({cx - a, cy - a * 0.5f}, {cx, cy + a * 0.5f}, v->dcBrush, stroke, nullptr);
    v->dcRt->DrawLine({cx, cy + a * 0.5f}, {cx + a, cy - a * 0.5f}, v->dcBrush, stroke, nullptr);
    if (v->dcRt->EndDraw(nullptr, nullptr) == D2DERR_RECREATE_TARGET) {
        release(v->dcBrush);
        release(v->dcRt);
    }
}

LRESULT CALLBACK comboProc(HWND combo, UINT msg, WPARAM wp, LPARAM lp) {
    auto* v = reinterpret_cast<View*>(GetWindowLongPtrW(combo, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(combo, &ps);
            paintComboField(v, combo, dc);
            EndPaint(combo, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_ENABLE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(combo, nullptr, FALSE);
            break;
        default:
            break;
    }
    return CallWindowProcW(v->comboBaseProc, combo, msg, wp, lp);
}

HWND makeCombo(View* v, int id) {
    HWND combo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST
                                     | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                 0, 0, 10, 10, v->hwnd, (HMENU)(INT_PTR)id, moduleHandle(),
                                 nullptr);
    if (!combo) return nullptr;
    // Classic subclassing: SetWindowSubclass needs comctl32 v6, which hosts
    // without a manifest don't load, and a missing import would keep the
    // whole plugin from loading.
    SetWindowLongPtrW(combo, GWLP_USERDATA, (LONG_PTR)v);
    v->comboBaseProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(combo, GWLP_WNDPROC, (LONG_PTR)comboProc));
    int count = id == kRootComboId ? 12 : scale::kTypeCount;
    for (int i = 0; i < count; ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)comboItemText(id, i).c_str());
    }
    return combo;
}

// ------------------------------------------------------------
// Tooltips
// ------------------------------------------------------------

const std::wstring* tipText(View* v, UINT_PTR id) {
    if (id == kUseTipId) return &v->useButton.tip;
    if (id == kResetTipId) return &v->resetButton.tip;
    if (id >= 1 && id <= v->pads.size()) return &v->pads[id - 1].tip;
    return nullptr;
}

void syncTool(View* v, UINT_PTR id, const D2D1_RECT_F& r, bool add) {
    TOOLINFOW ti{};
    // V2 size: the full struct is rejected by comctl32 v5, which hosts
    // without a v6 manifest load.
    ti.cbSize = TTTOOLINFOW_V2_SIZE;
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = v->hwnd;
    ti.uId = id;
    ti.rect = toPixels(v, r);
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(v->tooltip, add ? TTM_ADDTOOLW : TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
}

void syncTools(View* v, bool add) {
    if (!v->tooltip) return;
    for (size_t i = 0; i < v->pads.size(); ++i) syncTool(v, i + 1, v->pads[i].r, add);
    syncTool(v, kUseTipId, v->useButton.r, add);
    syncTool(v, kResetTipId, v->resetButton.r, add);
}

// ------------------------------------------------------------
// Scale factor
// ------------------------------------------------------------

void createFormats(View* v) {
    release(v->fCombo);
    if (!v->fLabel) {
        v->fLabel    = makeFormat(12, DWRITE_FONT_WEIGHT_MEDIUM);
        v->fOctave   = makeFormat(8, DWRITE_FONT_WEIGHT_MEDIUM);
        v->fHist     = makeFormat(10, DWRITE_FONT_WEIGHT_MEDIUM);
        v->fNumeral  = makeFormat(15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        v->fPadName  = makeFormat(10, DWRITE_FONT_WEIGHT_MEDIUM);
        v->fVariant  = makeFormat(9, DWRITE_FONT_WEIGHT_MEDIUM);
        v->fDetected = makeFormat(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        v->fButton   = makeFormat(11, DWRITE_FONT_WEIGHT_NORMAL);
    }
    v->fCombo = makeFormat(px(v, 12), DWRITE_FONT_WEIGHT_NORMAL);
}

// Applies v->scale to everything sized in pixels.
void applyScaleFactor(View* v) {
    createFormats(v);
    if (!v->hwnd) return;

    SetWindowPos(v->hwnd, nullptr, 0, 0, std::lround(px(v, kWidth)), std::lround(px(v, kHeight)),
                 SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    if (v->rt) v->rt->SetDpi(96.0f * (float)v->scale, 96.0f * (float)v->scale);

    for (HWND combo : {v->rootCombo, v->typeCombo}) {
        const D2D1_RECT_F& r = combo == v->rootCombo ? v->rootComboR : v->typeComboR;
        RECT p = toPixels(v, r);
        // The height of a drop-down list combo is that of the open list.
        SetWindowPos(combo, nullptr, p.left, p.top, p.right - p.left, std::lround(px(v, 300)),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, comboFieldHeight(v));
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, comboItemHeight(v));
    }
    syncTools(v, false);
    invalidate(v);
}

// ------------------------------------------------------------
// State updates
// ------------------------------------------------------------

void fitChordFont(View* v) {
    // Measure once at full size; glyph widths scale linearly with the font size.
    const float fullSize = 72;
    IDWriteTextFormat* full = makeFormat(fullSize, DWRITE_FONT_WEIGHT_BOLD);
    float fullW = full ? textWidth(full, v->chordText) : 0;
    float maxW = (v->chordR.right - v->chordR.left) - 8;
    float size = fullSize;
    while (fullW * size / fullSize > maxW && size > 16) size -= 4;

    if (size == fullSize) {
        release(v->fChord);
        v->fChord = full;
    } else {
        release(full);
        if (size != v->chordFontSize || !v->fChord) {
            release(v->fChord);
            v->fChord = makeFormat(size, DWRITE_FONT_WEIGHT_BOLD);
        }
    }
    v->chordFontSize = size;
}

bool updateChordPads(View* v, int root, int type) {
    static const wchar_t* const kPlainNumerals[7] = {L"I", L"II", L"III", L"IV", L"V", L"VI", L"VII"};
    static const wchar_t* const kVariantLabels[scale::kVariantCount] = {L"", L"sus2", L"sus4", L"7", L"9"};
    // Called on every poll; relabel only on change.
    if (root == v->padsRoot && type == v->padsType) return false;
    v->padsRoot = root;
    v->padsType = type;
    for (Pad& pad : v->pads) {
        scale::Chord c;
        bool ok = scale::diatonicChord(root, type, pad.degree,
                                       (scale::ChordVariant)pad.variant, &c);
        std::wstring name = ok ? widen(c.name) : L"";

        if (pad.variant == (int)scale::ChordVariant::Triad) {
            scale::Triad t;
            if (ok && scale::diatonicTriad(root, type, pad.degree, &t)) {
                pad.numeral = widen(scale::romanNumeral(pad.degree, t.quality));
                pad.name = name;
                pad.enabled = true;
            } else {
                // Pentatonic/blues/none: no seven diatonic triads.
                pad.numeral = kPlainNumerals[pad.degree];
                pad.name = L"—";
                pad.enabled = false;
            }
        } else {
            // Disabled when the scale can't form it (e.g. E sus2 in C major).
            pad.numeral = kVariantLabels[pad.variant];
            pad.name.clear();
            pad.enabled = ok;
        }

        pad.tip = ok ? name + L" — click to play, drag into your DAW" : L"";
    }
    return true;
}

bool applyScale(View* v, int root, int type) {
    bool changed = root != v->curRoot || type != v->curType;
    v->curRoot = root;
    v->curType = type;
    if (v->rootCombo && SendMessageW(v->rootCombo, CB_GETCURSEL, 0, 0) != root) {
        SendMessageW(v->rootCombo, CB_SETCURSEL, root, 0);
    }
    if (v->typeCombo && SendMessageW(v->typeCombo, CB_GETCURSEL, 0, 0) != type) {
        SendMessageW(v->typeCombo, CB_SETCURSEL, type, 0);
    }
    if (v->rootCombo && (IsWindowEnabled(v->rootCombo) != FALSE) != (type != 0)) {
        EnableWindow(v->rootCombo, type != 0);
    }

    uint16_t mask = scale::pitchClassMask(root, type);
    if (mask != v->kbMask || root != v->kbRoot) {
        v->kbMask = mask;
        v->kbRoot = root;
        changed = true;
    }
    changed |= updateChordPads(v, root, type);
    return changed;
}

bool updateDetected(View* v, int root, int type, bool early, int count) {
    v->detectedRoot = root;
    v->detectedType = type;

    std::wstring name;
    if (type > 0) {
        name = widen(scale::kRootNames[root]) + L" " + widen(scale::kTypes[type].name)
             + (early ? L"?" : L"");
    } else if (count == 0) {
        name = L"—";
    } else {
        name = L"Play more notes…";
    }
    // Tentative guesses are dimmer than a full detection.
    D2D1_COLOR_F color = type == 0 ? kDimText : early ? gray(0.78f) : kWhite;

    bool changed = name != v->detectedText || !sameColor(color, v->detectedColor)
                || v->useButton.enabled != (type > 0) || v->resetButton.enabled != (count > 0);
    v->detectedText = name;
    v->detectedColor = color;
    v->useButton.enabled = type > 0;
    v->resetButton.enabled = count > 0;
    return changed;
}

void pollTick(View* v) {
    if (!v->cb.getSnapshot) return;
    ChordSnapshot snap;
    std::memset(&snap, 0, sizeof(snap));
    v->cb.getSnapshot(v->cb.userData, &snap);
    bool changed = false;

    std::wstring s = widen(snap.name);
    if (s.empty()) s = L"—";
    bool isChord = snap.isChord != 0;
    if (s != v->chordText) {
        v->chordText = s;
        fitChordFont(v);
        changed = true;
    }

    D2D1_COLOR_F color = snap.noteCount == 0 ? kWhite
                       : isChord             ? rgb(0.60f, 0.85f, 1.00f)
                                             : rgb(1.00f, 0.82f, 0.45f);
    if (!sameColor(color, v->chordColor)) {
        v->chordColor = color;
        changed = true;
    }

    std::bitset<128> held;
    for (int i = 0; i < snap.noteCount && i < (int)sizeof(snap.notes); ++i) {
        held.set(snap.notes[i] & 0x7F);
    }
    if (held != v->held || isChord != v->chordRecognized) {
        v->held = held;
        v->chordRecognized = isChord;
        changed = true;
    }

    // Plugin state is the source of truth (e.g. after the host loads a project).
    if (snap.scaleRoot <= 11 && snap.scaleType < scale::kTypeCount) {
        changed |= applyScale(v, snap.scaleRoot, snap.scaleType);
    }

    int detRoot = snap.detectedRoot <= 11 ? snap.detectedRoot : 0;
    int detType = snap.detectedType < scale::kTypeCount ? snap.detectedType : 0;
    changed |= updateDetected(v, detRoot, detType, snap.detectedEarly != 0, snap.historyCount);

    // Bars are coloured by the detected scale, so the histogram shows how well it fits.
    uint16_t mask = scale::pitchClassMask(detRoot, detType);
    if (std::memcmp(snap.histogram, v->histCounts, sizeof(v->histCounts)) != 0
        || mask != v->histMask || detRoot != v->histRoot) {
        std::memcpy(v->histCounts, snap.histogram, sizeof(v->histCounts));
        v->histMask = mask;
        v->histRoot = detRoot;
        changed = true;
    }

    if (changed) invalidate(v);
}

// ------------------------------------------------------------
// Chord pad dragging
// ------------------------------------------------------------

fs::path dragFileRoot() {
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return fs::path(std::wstring(buf, n)) / L"ChordAnalyzer";
}

// Drops drag files from earlier sessions. Anything under a day old is kept
// in case a host reads the file lazily after the drop.
void removeStaleDragFiles() {
    std::error_code ec;
    auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
    for (fs::directory_iterator it(dragFileRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        auto modified = it->last_write_time(entryEc);
        if (!entryEc && modified < cutoff) fs::remove_all(it->path(), entryEc);
    }
}

// Writes a .mid file holding the pad's chord; empty if there is nothing to drag.
std::wstring midiFileForPad(View* v, const Pad& pad) {
    scale::Chord c;
    if (!scale::diatonicChord(v->curRoot, v->curType, pad.degree,
                              (scale::ChordVariant)pad.variant, &c)) {
        return {};
    }

    int keys[scale::kMaxChordNotes];
    scale::chordKeys(v->curRoot, c, keys);
    std::vector<uint8_t> bytes =
        midifile::chordFile(keys, c.count, (uint8_t)scale::kChordVelocity, c.asciiName);

    // A fresh directory per drag keeps the file name clean ("Cm.mid", which
    // hosts use as the clip name) without clobbering a file a host may
    // still be reading from an earlier drag.
    GUID guid;
    wchar_t guidText[64];
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, guidText, 64)) return {};
    std::wstring dirName(guidText + 1, 36); // strip the braces

    std::error_code ec;
    fs::path dir = dragFileRoot() / dirName;
    fs::create_directories(dir, ec);
    if (ec) return {};
    fs::path file = dir / (widen(c.asciiName) + L".mid");
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    out.close();
    if (!out) return {};
    return file.wstring();
}

// Modal: returns once the drop has happened or was cancelled.
void dragFile(View* v, const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) return;
    IShellFolder* folder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(SHBindToParent(pidl, IID_IShellFolder, (void**)&folder, &child))) {
        // The shell's data object offers CF_HDROP plus the shell formats
        // Explorer uses, which is what DAWs accept for file drops.
        IDataObject* data = nullptr;
        if (SUCCEEDED(folder->GetUIObjectOf(v->hwnd, 1, &child, IID_IDataObject, nullptr,
                                            (void**)&data))) {
            DWORD effect = 0;
            SHDoDragDrop(v->hwnd, data, nullptr, DROPEFFECT_COPY, &effect);
            data->Release();
        }
        folder->Release();
    }
    ILFree(pidl);
}

// ------------------------------------------------------------
// Input
// ------------------------------------------------------------

void releasePad(View* v) {
    if (v->pressedPad < 0) return;
    v->pressedPad = -1;
    invalidate(v);
    if (v->cb.releaseChord) v->cb.releaseChord(v->cb.userData);
}

void useDetectedScale(View* v) {
    if (v->detectedType <= 0) return;
    applyScale(v, v->detectedRoot, v->detectedType);
    if (v->cb.setScale) {
        v->cb.setScale(v->cb.userData, (uint8_t)v->detectedRoot, (uint8_t)v->detectedType);
    }
    invalidate(v);
}

void resetHistory(View* v) {
    if (v->cb.resetHistory) v->cb.resetHistory(v->cb.userData);
    pollTick(v);
}

void scaleSelectionChanged(View* v) {
    int root = (int)SendMessageW(v->rootCombo, CB_GETCURSEL, 0, 0);
    int type = (int)SendMessageW(v->typeCombo, CB_GETCURSEL, 0, 0);
    if (root < 0 || type < 0) return;
    applyScale(v, root, type);
    if (v->cb.setScale) v->cb.setScale(v->cb.userData, (uint8_t)root, (uint8_t)type);
    invalidate(v);
}

void onMouseDown(View* v, POINT p) {
    float x = p.x / (float)v->scale, y = p.y / (float)v->scale;
    for (size_t i = 0; i < v->pads.size(); ++i) {
        const Pad& pad = v->pads[i];
        if (!pad.enabled || !contains(pad.r, x, y)) continue;
        v->pressedPad = (int)i;
        v->dragStarted = false;
        v->downPt = p;
        SetCapture(v->hwnd);
        invalidate(v);
        if (v->cb.playChord) v->cb.playChord(v->cb.userData, pad.degree, pad.variant);
        return;
    }
    for (Button* b : {&v->useButton, &v->resetButton}) {
        if (!b->enabled || !contains(b->r, x, y)) continue;
        v->pressedButton = b;
        SetCapture(v->hwnd);
        invalidate(v);
        return;
    }
}

void onMouseMove(View* v, POINT p) {
    if (v->pressedPad < 0 || v->dragStarted) return;
    if (std::hypot(p.x - v->downPt.x, p.y - v->downPt.y) < px(v, 4)) return;

    // Moving turns the click into a drag: stop the preview, hand out a .mid file.
    v->dragStarted = true;
    Pad pad = v->pads[v->pressedPad];
    releasePad(v);
    ReleaseCapture();
    std::wstring path = midiFileForPad(v, pad);
    if (!path.empty()) dragFile(v, path);
}

void onMouseUp(View* v, POINT p) {
    if (Button* b = v->pressedButton) {
        v->pressedButton = nullptr;
        invalidate(v);
        if (contains(b->r, p.x / (float)v->scale, p.y / (float)v->scale)) {
            if (b == &v->useButton) useDetectedScale(v);
            else resetHistory(v);
        }
    }
    releasePad(v);
    if (GetCapture() == v->hwnd) ReleaseCapture();
}

// ------------------------------------------------------------
// Window
// ------------------------------------------------------------

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* v = static_cast<View*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        v->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)v);
    }
    auto* v = reinterpret_cast<View*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!v) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            paint(v);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (v->rt) v->rt->Resize(D2D1_SIZE_U{LOWORD(lp), HIWORD(lp)});
            return 0;
        case WM_TIMER:
            if (wp == kPollTimer) pollTick(v);
            return 0;
        case WM_LBUTTONDOWN:
            onMouseDown(v, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_MOUSEMOVE:
            onMouseMove(v, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_LBUTTONUP:
            onMouseUp(v, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_CAPTURECHANGED:
            // Lost the mouse (e.g. the host took focus): don't leave a chord hanging.
            releasePad(v);
            if (v->pressedButton) {
                v->pressedButton = nullptr;
                invalidate(v);
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == CBN_SELCHANGE) scaleSelectionChanged(v);
            // Hand the keyboard back to the host, or its shortcuts (and the
            // arrow keys, which would change the scale) stay with the combo.
            if (HIWORD(wp) == CBN_CLOSEUP) SetFocus(GetAncestor(hwnd, GA_ROOT));
            return 0;
        case WM_MEASUREITEM: {
            auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mis->CtlType != ODT_COMBOBOX) break;
            mis->itemHeight = mis->itemID == (UINT)-1 ? comboFieldHeight(v) : comboItemHeight(v);
            return TRUE;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis->CtlType != ODT_COMBOBOX) break;
            drawComboItem(v, dis);
            return TRUE;
        }
        case WM_CTLCOLORLISTBOX: {
            // Background of the open list beyond the owner-drawn items.
            HDC dc = (HDC)wp;
            SetBkColor(dc, RGB(33, 33, 41));
            SetTextColor(dc, RGB(235, 235, 235));
            return (LRESULT)v->listBrush;
        }
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->hwndFrom == v->tooltip && hdr->code == TTN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMTTDISPINFOW*>(lp);
                const std::wstring* tip = tipText(v, hdr->idFrom);
                di->lpszText = const_cast<wchar_t*>(tip ? tip->c_str() : L"");
                return 0;
            }
            break;
        }
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            v->hwnd = nullptr;
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool startup() {
    if (gViewCount++ > 0) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                                 nullptr, (void**)&gD2D))
        || FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      (IUnknown**)&gDWrite))) {
        release(gD2D);
        gViewCount--;
        return false;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // Unique per loaded module, so two copies of the plugin can't clash.
    swprintf(gClassName, 64, L"ChordAnalyzerView_%p", (void*)moduleHandle());
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = moduleHandle();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = gClassName;
    RegisterClassExW(&wc);
    return true;
}

void shutdown() {
    if (--gViewCount > 0) return;
    UnregisterClassW(gClassName, moduleHandle());
    release(gDWrite);
    release(gD2D);
}

bool createWindow(View* v, HWND parent) {
    HWND hwnd = CreateWindowExW(0, gClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                0, 0, std::lround(px(v, kWidth)), std::lround(px(v, kHeight)),
                                parent, nullptr, moduleHandle(), v);
    if (!hwnd) return false;

    v->listBrush = CreateSolidBrush(RGB(33, 33, 41));
    v->rootCombo = makeCombo(v, kRootComboId);
    v->typeCombo = makeCombo(v, kTypeComboId);

    v->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                 WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                 hwnd, nullptr, moduleHandle(), nullptr);
    syncTools(v, true);
    applyScaleFactor(v);

    applyScale(v, v->curRoot, v->curType);
    pollTick(v);
    SetTimer(hwnd, kPollTimer, kPollMs, nullptr);
    return true;
}

} // namespace

// ------------------------------------------------------------
// C entry points
// ------------------------------------------------------------

extern "C" {

void* chordGuiCreate(uint32_t w, uint32_t h, ChordGuiCallbacks callbacks) {
    (void)w;
    (void)h;
    if (!startup()) return nullptr;
    auto* v = new View();
    v->cb = callbacks;
    // Drag and drop needs OLE on this (the host's main) thread.
    HRESULT hr = OleInitialize(nullptr);
    v->oleInitialized = SUCCEEDED(hr);

    for (int d = 0; d < 7; ++d) {
        for (int var = 0; var < scale::kVariantCount; ++var) {
            Pad p;
            p.degree = d;
            p.variant = var;
            v->pads.push_back(p);
        }
    }
    v->useButton.title = L"Use";
    v->useButton.tip = L"Select the detected scale";
    v->resetButton.title = L"Reset";
    v->resetButton.tip = L"Clear the note histogram";

    layout(v);
    createFormats(v);
    fitChordFont(v);
    applyScale(v, 0, 0);
    updateDetected(v, 0, 0, false, 0);
    removeStaleDragFiles();
    return v;
}

void chordGuiDestroy(void* view) {
    if (!view) return;
    auto* v = static_cast<View*>(view);
    if (v->hwnd) {
        KillTimer(v->hwnd, kPollTimer);
        DestroyWindow(v->hwnd); // takes the combos and the tooltip with it
    }
    discardTarget(v);
    release(v->dcBrush);
    release(v->dcRt);
    for (IDWriteTextFormat** f : {&v->fLabel, &v->fOctave, &v->fHist, &v->fNumeral, &v->fPadName,
                                  &v->fVariant, &v->fDetected, &v->fButton, &v->fChord,
                                  &v->fCombo}) {
        release(*f);
    }
    if (v->listBrush) DeleteObject(v->listBrush);
    if (v->oleInitialized) OleUninitialize();
    delete v;
    shutdown();
}

bool chordGuiSetParent(void* view, void* parent) {
    if (!view || !parent) return false;
    auto* v = static_cast<View*>(view);
    if (!v->hwnd) return createWindow(v, static_cast<HWND>(parent));
    SetParent(v->hwnd, static_cast<HWND>(parent));
    SetWindowPos(v->hwnd, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

void chordGuiGetPreferredSize(uint32_t* w, uint32_t* h) {
    if (w) *w = (uint32_t)kWidth;
    if (h) *h = (uint32_t)kHeight;
}

void chordGuiSetScale(void* view, double scale) {
    if (!view || !(scale > 0)) return;
    auto* v = static_cast<View*>(view);
    if (scale == v->scale) return;
    v->scale = scale;
    applyScaleFactor(v);
}

double chordGuiSystemScale() {
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi / 96.0 : 1.0;
}

} // extern "C"
