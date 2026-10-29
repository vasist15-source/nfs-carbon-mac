// settings_menu_draw.cpp - draws the tabbed settings menu (settings_menu.h) as a straight-alpha
// RGBA page at the size the presenter asks for (the game's rectangle on screen), in the kit's
// own Open Sans. It is only redrawn when the menu's content or the size changed; the presenter
// keeps the last texture otherwise. Called on the thread that seals a frame, so it uses its own
// glyph cache (gdi's is the guest's) under its own lock.
//
// A host can give it a look of its own (mods_menu_set_style): the macOS host puts a native glass
// panel behind the page, so the page leaves the panel clear and draws only what is on it, with
// rounded shapes, switches for on/off rows and the system's accent colour, and draws its text in
// the system font (mods_menu_set_text_backend).
#include "settings_menu.h"
#include "mods_internal.h"
#include "../runtime/gdi32_truetype.h"
#include "../runtime/mods_seam.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <mutex>
#include <tuple>
#include <vector>

namespace {

struct Rgba {
    uint8_t r, g, b, a;
};
const Rgba kDim = {0, 0, 0, 150};
const Rgba kPanel = {18, 20, 24, 238};
const Rgba kEdge = {64, 68, 78, 255};
const Rgba kInk = {236, 238, 242, 255};
const Rgba kSoft = {168, 174, 184, 255};
const Rgba kFaint = {112, 116, 124, 255};
const Rgba kAccent = {255, 168, 24, 255};
const Rgba kSelect = {255, 255, 255, 26};

// The host's look and text, set by mods_menu_set_style / mods_menu_set_text_backend; under g_mu.
bool g_glass = false;
Rgba g_glass_accent = {10, 132, 255, 255};
const MenuTextBackend *g_text = nullptr;
float g_panel[5] = {0, 0, 0, 0, 0}; // x0 y0 x1 y1 as fractions of the page, radius / page height
bool g_panel_valid = false;

struct Canvas {
    int w, h;
    std::vector<uint8_t> *px;
    void blend(int x, int y, Rgba c, int cov) { // cov 0-255
        if (x < 0 || y < 0 || x >= w || y >= h || cov <= 0)
            return;
        uint8_t *p = px->data() + (size_t(y) * w + x) * 4;
        const int a = c.a * cov / 255;
        p[0] = uint8_t((c.r * a + p[0] * (255 - a)) / 255);
        p[1] = uint8_t((c.g * a + p[1] * (255 - a)) / 255);
        p[2] = uint8_t((c.b * a + p[2] * (255 - a)) / 255);
        p[3] = uint8_t(a + p[3] * (255 - a) / 255);
    }
    void fill(int x0, int y0, int x1, int y1, Rgba c) {
        x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, w), y1 = std::min(y1, h);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                blend(x, y, c, 255);
    }
};

// UTF-8 to codepoints; a malformed byte becomes '?'.
std::vector<uint32_t> decode(const std::string &s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n == 0)) {
            out.push_back('?');
            ++i;
            continue;
        }
        uint32_t cp = n == 0 ? c : n == 1 ? (c & 31) : n == 2 ? (c & 15) : (c & 7);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            const uint8_t d = uint8_t(s[i + k]);
            if ((d >> 6) != 2)
                ok = false;
            cp = (cp << 6) | (d & 63);
        }
        out.push_back(ok ? cp : '?');
        i += ok ? n + 1 : 1;
    }
    return out;
}

struct Font {
    const gdi::TrueTypeFace *face = nullptr;
    void *native = nullptr; // the host's font (g_text), which then draws and measures the text
    double scale = 0;
    int ascent = 0, height = 0;
};

std::mutex g_mu;
std::map<std::tuple<const gdi::TrueTypeFace *, double, uint32_t>, gdi::TrueTypeGlyph> g_glyphs;

const gdi::TrueTypeGlyph &glyph(const Font &f, uint32_t cp) {
    auto key = std::make_tuple(f.face, f.scale, cp);
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end())
        return it->second;
    if (g_glyphs.size() > 2048)
        g_glyphs.clear();
    gdi::TrueTypeGlyph g;
    gdi::truetype_render_glyph(f.face, f.scale, cp, &g);
    return g_glyphs.emplace(key, std::move(g)).first->second;
}

// A whole string drawn by the host's text backend, cached like the glyphs.
struct NativeRun {
    int w = 0, h = 0, x = 0, y = 0, advance = 0;
    std::vector<uint8_t> coverage;
};
std::map<std::pair<void *, std::string>, NativeRun> g_runs;

const NativeRun &native_run(const Font &f, const std::string &s) {
    auto key = std::make_pair(f.native, s);
    auto it = g_runs.find(key);
    if (it != g_runs.end())
        return it->second;
    if (g_runs.size() > 1024)
        g_runs.clear();
    NativeRun r;
    r.advance = g_text->measure(f.native, s.c_str());
    uint8_t *cov = nullptr;
    if (g_text->render(f.native, s.c_str(), &cov, &r.w, &r.h, &r.x, &r.y) && cov && r.w > 0 && r.h > 0)
        r.coverage.assign(cov, cov + size_t(r.w) * size_t(r.h));
    else
        r.w = r.h = 0;
    std::free(cov);
    return g_runs.emplace(key, std::move(r)).first->second;
}

Font font(int px, bool bold) {
    Font f;
    if (g_text && (f.native = g_text->font(std::max(px, 6), bold ? 600 : 400))) {
        g_text->metrics(f.native, &f.ascent, &f.height);
        return f;
    }
    f.face = gdi::truetype_windows_substitute("Segoe UI", bold ? 600 : 400);
    if (!f.face)
        return f;
    f.scale = gdi::truetype_scale(f.face, -std::max(px, 6));
    const gdi::TrueTypeMetrics m = gdi::truetype_metrics(f.face, f.scale);
    f.ascent = m.ascent;
    f.height = m.height;
    return f;
}

int measure(const Font &f, const std::vector<uint32_t> &cps) {
    if (!f.face)
        return 0;
    int x = 0;
    for (size_t i = 0; i < cps.size(); ++i) {
        x += gdi::truetype_advance(f.face, f.scale, cps[i]);
        if (i + 1 < cps.size())
            x += gdi::truetype_kern(f.face, f.scale, cps[i], cps[i + 1]);
    }
    return x;
}
int measure(const Font &f, const std::string &s) {
    if (f.native)
        return native_run(f, s).advance;
    return measure(f, decode(s));
}

// Text with its top at y. Returns the advance. Clipped at max_x when given.
int text(Canvas &c, const Font &f, int x, int y, const std::string &s, Rgba ink, int max_x = 1 << 30) {
    if (f.native) {
        const NativeRun &r = native_run(f, s);
        const int base = y + f.ascent;
        for (int gy = 0; gy < r.h; ++gy)
            for (int gx = 0; gx < r.w; ++gx) {
                const int px = x + r.x + gx;
                if (px < max_x)
                    c.blend(px, base + r.y + gy, ink, r.coverage[size_t(gy) * r.w + gx]);
            }
        return r.advance;
    }
    if (!f.face)
        return 0;
    const std::vector<uint32_t> cps = decode(s);
    const int x0 = x, base = y + f.ascent;
    for (size_t i = 0; i < cps.size(); ++i) {
        const gdi::TrueTypeGlyph &g = glyph(f, cps[i]);
        for (int gy = 0; gy < g.h; ++gy)
            for (int gx = 0; gx < g.w; ++gx) {
                const int px = x + g.x + gx;
                if (px < max_x)
                    c.blend(px, base + g.y + gy, ink, g.coverage[size_t(gy) * g.w + gx]);
            }
        x += gdi::truetype_advance(f.face, f.scale, cps[i]);
        if (i + 1 < cps.size())
            x += gdi::truetype_kern(f.face, f.scale, cps[i], cps[i + 1]);
    }
    return x - x0;
}

// Word-wrapped lines no wider than width.
std::vector<std::string> wrap(const Font &f, const std::string &s, int width) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush_word = [&] {
        if (word.empty())
            return;
        const std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && measure(f, trial) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = trial;
        }
        word.clear();
    };
    for (char ch : s) {
        if (ch == ' ' || ch == '\n') {
            flush_word();
            if (ch == '\n') {
                lines.push_back(line);
                line.clear();
            }
        } else {
            word += ch;
        }
    }
    flush_word();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

} // namespace

namespace {
std::vector<uint8_t> g_page;

// Where the last drawn page put what the pointer can reach, as fractions of the page, so a pointer
// event (on the input thread) finds what it is over without redrawing anything. Under g_mu.
enum HitKind : uint8_t { HIT_TAB, HIT_CLOSE, HIT_ACTION, HIT_DECREASE, HIT_INCREASE, HIT_ROW };
struct Hit {
    float x0, y0, x1, y1;
    HitKind kind;
    size_t index; // the tab, or the line in the snapshot
};
std::vector<Hit> g_hits;
int g_hits_tab = -1;
// The list's scroll position persists between draws and moves only as far as keeps the selected
// line in view, so pointing at a visible row never scrolls the list under the pointer.
size_t g_first = 0;
int g_first_tab = -1;
// What the pointer is over that the menu model does not know about (a page name, Close), and a
// counter of such changes for the redraw key.
int g_hover_kind = -1;
size_t g_hover_index = 0;
uint64_t g_view_serial = 0;
int g_wheel = 0; // a trackpad's partial notches, accumulated

// A small filled triangle pointing up (dir < 0) or down, centred on (cx, cy).
void triangle(Canvas &c, int cx, int cy, int half, int dir, Rgba ink) {
    for (int r = 0; r <= half; ++r) {
        const int y = cy + (dir < 0 ? r - half / 2 : half / 2 - r); // r = 0 is the tip
        for (int x = cx - r; x <= cx + r; ++x)
            c.blend(x, y, ink, 255);
    }
}
void outline(Canvas &c, int x0, int y0, int x1, int y1, int t, Rgba ink) {
    c.fill(x0, y0, x1, y0 + t, ink);
    c.fill(x0, y1 - t, x1, y1, ink);
    c.fill(x0, y0, x0 + t, y1, ink);
    c.fill(x1 - t, y0, x1, y1, ink);
}
// A rounded rectangle with smooth edges (r up to half the shorter side: a capsule), filled, or
// only its edge `stroke` pixels wide.
void round_rect(Canvas &c, int x0, int y0, int x1, int y1, double r, Rgba ink, double stroke = 0) {
    r = std::max(0.0, std::min({r, (x1 - x0) / 2.0, (y1 - y0) / 2.0}));
    for (int y = std::max(y0, 0); y < std::min(y1, c.h); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, c.w); ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double dx = std::max({x0 + r - px, px - (x1 - r), 0.0});
            const double dy = std::max({y0 + r - py, py - (y1 - r), 0.0});
            // The distance to the edge, negative inside.
            const double d = dx > 0 || dy > 0 ? std::sqrt(dx * dx + dy * dy) - r
                                              : -std::min({px - x0, x1 - px, py - y0, y1 - py});
            double cov = std::clamp(0.5 - d, 0.0, 1.0);
            if (stroke > 0)
                cov = std::min(cov, std::clamp(0.5 + d + stroke, 0.0, 1.0));
            if (cov > 0)
                c.blend(x, y, ink, int(cov * 255 + 0.5));
        }
}
// An on/off switch, macOS style, its left edge at x and centred on cy.
void toggle(Canvas &c, int x, int cy, int w, int h, bool on, Rgba accent) {
    round_rect(c, x, cy - h / 2, x + w, cy + h - h / 2, h / 2.0, on ? accent : Rgba{255, 255, 255, 52});
    const int inset = std::max(2, h / 12), d = h - 2 * inset;
    const int kx = on ? x + w - inset - d : x + inset, ky = cy - h / 2 + inset;
    round_rect(c, kx, ky + 1, kx + d, ky + d + 1, d / 2.0, Rgba{0, 0, 0, 60}); // its shadow
    round_rect(c, kx, ky, kx + d, ky + d, d / 2.0, Rgba{255, 255, 255, 255});
}
} // namespace

namespace {
// The colours of one look. The glass look's are white at various strengths, so the glass shows
// through, with the host's accent.
struct Palette {
    Rgba ink, soft, faint, edge, select, accent, row;
};
Palette palette() {
    if (!g_glass)
        return {kInk, kSoft, kFaint, kEdge, kSelect, kAccent, {214, 216, 222, 255}};
    return {{255, 255, 255, 255}, {255, 255, 255, 190}, {255, 255, 255, 150}, {255, 255, 255, 46},
            {255, 255, 255, 40},  g_glass_accent,        {255, 255, 255, 235}};
}
} // namespace

int mods_menu_render(int w, int h, uint64_t *generation, const uint8_t **pixels) {
    if (!generation || !pixels || w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return 0;
    MenuSnapshot m;
    if (!mods_menu_snapshot(&m))
        return 0;
    std::lock_guard lock(g_mu);
    const uint64_t key = (m.generation ^ (uint64_t(uint32_t(w)) << 40) ^ (uint64_t(uint32_t(h)) << 20)) +
                         g_view_serial * 0x9e3779b97f4a7c15ull + (g_glass ? 0x5bd1e995ull : 0);
    if (*generation == key)
        return 1; // unchanged: the caller keeps what it has
    const Palette P = palette();
    const bool glass = g_glass;
    std::vector<uint8_t> *rgba = &g_page;
    rgba->assign(size_t(w) * size_t(h) * 4, 0);
    Canvas c{w, h, rgba};
    if (!glass)
        c.fill(0, 0, w, h, kDim);
    std::vector<Hit> hits;
    auto hit = [&](int x0, int y0, int x1, int y1, HitKind kind, size_t index) {
        hits.push_back({float(x0) / w, float(y0) / h, float(x1) / w, float(y1) / h, kind, index});
    };
    const bool hover_tab_kind = g_hover_kind == HIT_TAB, hover_close = g_hover_kind == HIT_CLOSE;
    const bool hover_action_kind = g_hover_kind == HIT_ACTION;

    // Laid out on a 1920x1080 reference and scaled to fit the page.
    const double s = std::min(w / 1920.0, h / 1080.0);
    auto u = [s](double v) { return int(std::lround(v * s)); };
    const int pw = std::min(w - u(40), u(1240)), ph = std::min(h - u(40), u(940));
    const int px = (w - pw) / 2, py = (h - ph) / 2;
    if (glass) {
        // The host's glass goes here; the page leaves it clear.
        const int radius = u(44);
        g_panel[0] = float(px) / w, g_panel[1] = float(py) / h;
        g_panel[2] = float(px + pw) / w, g_panel[3] = float(py + ph) / h;
        g_panel[4] = float(radius) / h;
        g_panel_valid = true;
    } else {
        c.fill(px - u(2), py - u(2), px + pw + u(2), py + ph + u(2), kEdge);
        c.fill(px, py, px + pw, py + ph, kPanel);
    }
    const int left = px + u(56), right = px + pw - u(56);

    // Tabs: page names that look and act like buttons; on glass, a segmented control.
    const Font tab_font = font(u(glass ? 28 : 34), true);
    int tx = left;
    const int tab_y = py + u(34);
    const int seg_pad = u(26), seg_gap = glass ? u(4) : u(48);
    if (glass) {
        int track = 0;
        for (size_t i = 0; i < m.tabs.size(); ++i)
            track += measure(tab_font, m.tabs[i]) + 2 * seg_pad + (i ? seg_gap : 0);
        round_rect(c, left - u(6), tab_y - u(6), left + track + u(6), tab_y + tab_font.height + u(18), 1e9,
                   Rgba{255, 255, 255, 26});
        tx = left;
    }
    for (size_t i = 0; i < m.tabs.size(); ++i) {
        const bool on = int(i) == m.tab;
        const bool hover = !on && hover_tab_kind && g_hover_index == i;
        const int adv = measure(tab_font, m.tabs[i]);
        if (glass) {
            const int bx0 = tx, by0 = tab_y, bx1 = tx + adv + 2 * seg_pad, by1 = tab_y + tab_font.height + u(12);
            if (on || hover)
                round_rect(c, bx0, by0, bx1, by1, 1e9, on ? Rgba{255, 255, 255, 64} : Rgba{255, 255, 255, 26});
            text(c, tab_font, bx0 + seg_pad, tab_y + u(6), m.tabs[i], on ? P.ink : hover ? P.ink : P.soft);
            hit(bx0, by0, bx1, by1, HIT_TAB, i);
            tx = bx1 + seg_gap;
            continue;
        }
        const int bx0 = tx - u(16), by0 = tab_y - u(6), bx1 = tx + adv + u(16),
                  by1 = tab_y + tab_font.height + u(14);
        if (hover)
            c.fill(bx0, by0, bx1, by1, P.select);
        text(c, tab_font, tx, tab_y, m.tabs[i], on ? P.ink : hover ? P.soft : P.faint);
        if (on)
            c.fill(tx, tab_y + tab_font.height + u(8), tx + adv, tab_y + tab_font.height + u(12), P.accent);
        hit(bx0, by0, bx1, by1, HIT_TAB, i);
        tx += adv + u(48);
    }
    // Close, at the right of the tab bar, and how to change page beside it.
    const Font hint_font = font(u(20), false), close_font = font(u(22), true);
    const std::string close = "Close";
    const int cw = measure(close_font, close);
    const int cx1 = right, cx0 = right - cw - u(36);
    const int cy0 = tab_y + u(2), cy1 = cy0 + close_font.height + u(16);
    if (glass) {
        round_rect(c, cx0, cy0, cx1, cy1, 1e9, hover_close ? Rgba{255, 255, 255, 72} : Rgba{255, 255, 255, 34});
        text(c, close_font, cx0 + u(18), cy0 + u(8), close, P.ink);
    } else {
        if (hover_close)
            c.fill(cx0, cy0, cx1, cy1, P.select);
        outline(c, cx0, cy0, cx1, cy1, std::max(1, u(2)), hover_close ? P.soft : P.edge);
        text(c, close_font, cx0 + u(18), cy0 + u(8), close, hover_close ? P.ink : P.soft);
    }
    hit(cx0, cy0, cx1, cy1, HIT_CLOSE, 0);
    const std::string pages = glass ? "Tab or click to change page" : "Tab, or click a page name";
    const int pages_x = std::max(tx + u(12), cx0 - u(24) - measure(hint_font, pages));
    text(c, hint_font, pages_x, cy0 + (cy1 - cy0 - hint_font.height) / 2, pages, P.faint, cx0 - u(12));
    const int rule_y = tab_y + tab_font.height + u(glass ? 36 : 24);
    c.fill(left, rule_y, right, rule_y + std::max(1, u(1)), P.edge);

    // The footer: timing, help, status and prompts, from the bottom up.
    const Font help_font = font(u(22), false), small_font = font(u(20), false);
    const int bottom = py + ph - u(30);
    std::vector<std::string> prompts;
    for (size_t a = 0;;) {
        const size_t b = m.prompts.find('\n', a);
        prompts.push_back(m.prompts.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos)
            break;
        a = b + 1;
    }
    int fy = bottom;
    for (size_t i = prompts.size(); i-- > 0;) {
        fy -= small_font.height;
        text(c, small_font, left, fy, prompts[i], P.faint, right);
        fy -= u(4);
    }
    fy -= u(10);
    // While changes wait for Apply, Apply and Discard sit beside the status line as buttons, so the
    // pointer does not have to find them at the end of the list.
    std::vector<size_t> actions;
    for (size_t i = 0; i < m.lines.size(); ++i)
        if (m.lines[i].kind == MenuLine::ACTION && m.lines[i].enabled && !m.lines[i].list_only)
            actions.push_back(i);
    if (!m.status.empty() || !actions.empty()) {
        const Font button_font = font(u(22), true);
        const int bh = actions.empty() ? small_font.height : button_font.height + u(14);
        fy -= bh;
        int bx = right;
        for (size_t k = actions.size(); k-- > 0;) {
            const size_t i = actions[k];
            const bool hover = hover_action_kind && g_hover_index == i;
            const int bw = measure(button_font, m.lines[i].label) + u(36);
            bx -= bw;
            if (glass) {
                // Apply is the default button: filled with the accent.
                const bool primary = m.lines[i].label == "Apply";
                Rgba fill = primary ? P.accent : Rgba{255, 255, 255, 40};
                if (hover)
                    fill.a = uint8_t(std::min(255, fill.a + (primary ? 0 : 36)));
                round_rect(c, bx, fy, bx + bw, fy + bh, 1e9, fill);
                if (hover && primary)
                    round_rect(c, bx, fy, bx + bw, fy + bh, 1e9, Rgba{255, 255, 255, 40});
                text(c, button_font, bx + u(18), fy + u(7), m.lines[i].label, P.ink);
            } else {
                if (hover)
                    c.fill(bx, fy, bx + bw, fy + bh, P.select);
                outline(c, bx, fy, bx + bw, fy + bh, std::max(1, u(2)), hover ? P.ink : P.accent);
                text(c, button_font, bx + u(18), fy + u(7), m.lines[i].label, hover ? P.ink : P.accent);
            }
            hit(bx, fy, bx + bw, fy + bh, HIT_ACTION, i);
            bx -= u(14);
        }
        text(c, small_font, left, fy + (bh - small_font.height) / 2, m.status, P.accent, bx - u(10));
        fy -= u(8);
    }
    std::string timing;
    for (const MenuLine &l : m.lines)
        if (l.selected)
            timing = l.timing;
    const std::vector<std::string> help = wrap(help_font, m.help, right - left);
    const int help_lines = std::min<int>(int(help.size()), 4);
    const int help_h = help_lines * (help_font.height + u(4)) + (timing.empty() ? 0 : small_font.height + u(8));
    const int footer_top = fy - std::max(help_h, u(110)) - u(16);
    int hy = footer_top + u(16);
    if (!timing.empty()) {
        text(c, small_font, left, hy, timing, P.accent, right);
        hy += small_font.height + u(8);
    }
    for (int i = 0; i < help_lines; ++i) {
        text(c, help_font, left, hy, help[size_t(i)], P.soft, right);
        hy += help_font.height + u(4);
    }
    c.fill(left, footer_top, right, footer_top + std::max(1, u(1)), P.edge);

    // The rows. The view keeps its place and moves only to keep the selected line in sight.
    const Font row_font = font(u(26), false), head_font = font(u(21), true), value_font = font(u(26), true);
    const int row_h = u(46), head_h = u(54);
    const int list_top = rule_y + u(18), list_bottom = footer_top - u(10), avail = list_bottom - list_top;
    auto height_of = [&](const MenuLine &l) { return l.kind == MenuLine::HEADER ? head_h : row_h; };
    const size_t n = m.lines.size();
    auto fits = [&](size_t a, size_t b) {
        int used = 0;
        for (size_t i = a; i <= b && i < n; ++i)
            used += height_of(m.lines[i]);
        return used <= avail;
    };
    size_t sel = 0;
    for (size_t i = 0; i < n; ++i)
        if (m.lines[i].selected)
            sel = i;
    if (m.tab != g_first_tab) {
        g_first = 0;
        g_first_tab = m.tab;
    }
    size_t max_first = n ? n - 1 : 0; // the furthest scroll that still fills the list
    while (max_first > 0 && fits(max_first - 1, n - 1))
        --max_first;
    g_first = std::min(g_first, max_first);
    if (sel < g_first) {
        g_first = sel;
        if (sel > 0 && m.lines[sel - 1].kind == MenuLine::HEADER && fits(sel - 1, sel))
            g_first = sel - 1; // a section's header with its first row
    }
    while (g_first < sel && !fits(g_first, sel))
        ++g_first;
    const size_t first = g_first;
    int y = list_top;
    const int chevron_w = measure(value_font, "<") + u(14), gt_w = measure(value_font, ">");
    const int switch_w = u(58), switch_h = u(32);
    size_t last_drawn = first;
    for (size_t i = first; i < n; ++i) {
        const MenuLine &l = m.lines[i];
        const int lh = height_of(l);
        if (y + lh > list_bottom)
            break;
        last_drawn = i;
        if (l.kind == MenuLine::HEADER) {
            std::string upper = l.label;
            if (!glass) // the glass look keeps a section's own capitals, as macOS does
                for (char &ch : upper)
                    if (ch >= 'a' && ch <= 'z')
                        ch = char(ch - 32);
            text(c, head_font, left, y + lh - head_font.height - u(8), upper, P.soft, right);
            y += lh;
            continue;
        }
        if (l.selected) {
            if (glass) {
                round_rect(c, left - u(20), y + u(2), right + u(20), y + lh - u(2), u(14), P.select);
            } else {
                c.fill(left - u(20), y, right + u(20), y + lh, P.select);
                c.fill(left - u(20), y, left - u(14), y + lh, P.accent);
            }
        }
        const Rgba ink = !l.enabled ? P.faint : l.selected ? P.ink : P.row;
        const int ty = y + (lh - row_font.height) / 2;
        if (l.kind == MenuLine::ACTION) {
            text(c, value_font, left, ty, l.label, glass && l.enabled ? P.accent : ink, right);
            hit(left - u(20), y, right + u(20), y + lh, HIT_ROW, i);
            y += lh;
            continue;
        }
        // On glass, an on/off row is a switch; choosing the row (or clicking it) flips it.
        const bool is_switch = glass && l.adjustable && (l.value == "On" || l.value == "Off");
        if (is_switch) {
            hit(left - u(20), y, right + u(20), y + lh, HIT_ROW, i);
            const int sx = right - switch_w;
            toggle(c, sx, y + lh / 2, switch_w, switch_h, l.value == "On", l.enabled ? P.accent : P.faint);
            int lx_end = sx - u(40);
            if (l.pending) {
                const int d = u(10), dcx = sx - u(20), dcy = y + lh / 2;
                round_rect(c, dcx - d / 2, dcy - d / 2, dcx + d / 2, dcy + d / 2, d, P.accent);
                lx_end = dcx - u(20);
            }
            text(c, row_font, left, ty, l.label, ink, lx_end);
            y += lh;
            continue;
        }
        const int vw = measure(value_font, l.value);
        if (l.adjustable) {
            // The arrows' places whether or not they are showing: pointing at the row shows them.
            const int vx_sel = right - gt_w - u(14) - vw;
            hit(right - gt_w - u(18), y, right + u(20), y + lh, HIT_INCREASE, i);
            hit(vx_sel - chevron_w - u(18), y, vx_sel - u(4), y + lh, HIT_DECREASE, i);
        }
        hit(left - u(20), y, right + u(20), y + lh, HIT_ROW, i);
        int vx = right;
        if (l.selected && l.adjustable) {
            vx -= gt_w;
            text(c, value_font, vx, ty, ">", P.accent);
            vx -= u(14);
        }
        vx -= vw;
        text(c, value_font, vx, ty, l.value, l.pending ? P.accent : glass && !l.selected ? P.soft : ink);
        if (l.selected && l.adjustable)
            text(c, value_font, vx - chevron_w, ty, "<", P.accent);
        if (l.pending) {
            const int d = u(10), cx = vx - (l.selected && l.adjustable ? chevron_w : 0) - u(20), cy = y + lh / 2;
            for (int yy = -d / 2; yy <= d / 2; ++yy)
                for (int xx = -d / 2; xx <= d / 2; ++xx)
                    if (xx * xx + yy * yy <= d * d / 4)
                        c.blend(cx + xx, cy + yy, P.accent, 255);
        }
        text(c, row_font, left, ty, l.label, ink, vx - chevron_w - u(40));
        y += lh;
    }
    // More above or below: small arrows at the list's right edge.
    if (first > 0)
        triangle(c, right + u(36), list_top + u(10), u(9), -1, P.soft);
    if (n && last_drawn + 1 < n)
        triangle(c, right + u(36), list_bottom - u(10), u(9), +1, P.soft);
    g_hits = std::move(hits);
    g_hits_tab = m.tab;
    *generation = key;
    *pixels = g_page.data();
    return 2;
}

void mods_menu_set_text_backend(const MenuTextBackend *backend) {
    std::lock_guard lock(g_mu);
    g_text = backend && backend->font && backend->metrics && backend->measure && backend->render ? backend : nullptr;
    g_runs.clear();
    ++g_view_serial; // redraw with it
}

void mods_menu_set_style(int glass, uint8_t r, uint8_t g, uint8_t b) {
    std::lock_guard lock(g_mu);
    g_glass = glass != 0;
    g_glass_accent = {r, g, b, 255};
    g_panel_valid = false;
    ++g_view_serial;
}

int mods_menu_panel(float *x0, float *y0, float *x1, float *y1, float *radius) {
    std::lock_guard lock(g_mu);
    if (!g_panel_valid || !g_glass)
        return 0;
    *x0 = g_panel[0], *y0 = g_panel[1], *x1 = g_panel[2], *y1 = g_panel[3], *radius = g_panel[4];
    return 1;
}

// The pointer over the page, as fractions of the page (u, v in 0..1). button: -1 for motion,
// else 0 left / 1 right / 2 middle with down; wheel: WM_MOUSEWHEEL units, positive away from
// the user. Returns 1 (consumed) whenever the menu is open, so nothing reaches the game.
int mods_menu_pointer(float pu, float pv, int button, int down, int wheel) {
    if (!mods_menu_is_open())
        return 0;
    const Hit *found = nullptr;
    Hit copy{};
    int tab = -1;
    int steps = 0;
    {
        std::lock_guard lock(g_mu);
        tab = g_hits_tab;
        for (const Hit &h : g_hits)
            if (pu >= h.x0 && pu < h.x1 && pv >= h.y0 && pv < h.y1) {
                copy = h;
                found = &copy;
                break;
            }
        const int kind =
            found && (found->kind == HIT_TAB || found->kind == HIT_CLOSE || found->kind == HIT_ACTION) ? int(found->kind)
                                                                                                    : -1;
        const size_t index = found ? found->index : 0;
        if (kind != g_hover_kind || (kind >= 0 && index != g_hover_index)) {
            g_hover_kind = kind;
            g_hover_index = index;
            ++g_view_serial;
        }
        if (wheel) {
            g_wheel += wheel;
            while (g_wheel >= 120) {
                g_wheel -= 120;
                --steps;
            }
            while (g_wheel <= -120) {
                g_wheel += 120;
                ++steps;
            }
        }
    }
    // The model is called without the drawing lock held: the presenter takes them the other way.
    for (; steps < 0; ++steps)
        mods_menu_key(0xc8); // up
    for (; steps > 0; --steps)
        mods_menu_key(0xd0); // down
    if (wheel || !found)
        return 1;
    const bool row = found->kind == HIT_ROW || found->kind == HIT_DECREASE || found->kind == HIT_INCREASE;
    if (button < 0) { // motion: pointing at a row chooses it
        if (row)
            mods_menu_pointer_line(tab, found->index, MENU_POINT_HOVER);
        return 1;
    }
    if (!down || button != 0)
        return 1; // the press acts; its release and the other buttons do nothing
    switch (found->kind) {
    case HIT_TAB:
        mods_menu_set_tab(int(found->index));
        break;
    case HIT_CLOSE:
        mods_menu_close();
        break;
    case HIT_DECREASE:
        mods_menu_pointer_line(tab, found->index, MENU_POINT_DECREASE);
        break;
    case HIT_INCREASE:
        mods_menu_pointer_line(tab, found->index, MENU_POINT_INCREASE);
        break;
    case HIT_ROW:
        mods_menu_pointer_line(tab, found->index, MENU_POINT_ACTIVATE);
        break;
    case HIT_ACTION:
        mods_menu_pointer_line(tab, found->index, MENU_POINT_PRESS);
        break;
    }
    return 1;
}
