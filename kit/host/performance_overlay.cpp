#include "performance_overlay.h"
#include "perf_glass.h"

#include "../mods/mods_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
struct Canvas {
    std::vector<uint8_t> &px;
    int w, h;
    void fill(int x, int y, int fw, int fh, int r, int g, int b, int a) {
        // Premultiplied, as the HUD pipeline blends One / OneMinusSrcAlpha.
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min(w, x + fw), y1 = std::min(h, y + fh);
        for (int yy = y0; yy < y1; ++yy)
            for (int xx = x0; xx < x1; ++xx) {
                uint8_t *p = &px[(size_t(yy) * w + xx) * 4];
                p[0] = uint8_t(r * a / 255);
                p[1] = uint8_t(g * a / 255);
                p[2] = uint8_t(b * a / 255);
                p[3] = uint8_t(a);
            }
    }
    // 6x8 glyphs at 2x: 12 pixels per column, 16 per row.
    void text(int x, int y, const char *str, int r, int g, int b) {
        for (; *str; ++str, x += 12) {
            const uint8_t *glyph = mods_font6x8_glyph(*str);
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 6; ++col)
                    if (glyph[row] & (0x20 >> col))
                        fill(x + col * 2, y + row * 2, 2, 2, r, g, b, 255);
        }
    }
};
} // namespace

PerformanceOverlay::~PerformanceOverlay() {
    if (device_ && texture_)
        device_->destroy(texture_);
}

// Rasterize a pacing snapshot into the overlay texture. The graph uses a fixed
// 50 ms scale and marks samples exceeding the selected frame budget.
void PerformanceOverlay::update(gpu::Device *device, const FramePacingSnapshot &s, int mode,
                                int limit, const CaptureMarker &capture, uint64_t presenter_frame) {
    const int base_used = mode == 2 ? height : 88;
    const int used = base_used + (capture.enabled ? 80 : 0);
    pixels_.assign(size_t(width) * used * 4, 0);
    Canvas c{pixels_, width, used};
    c.fill(0, 0, width, used, 6, 9, 15, 224);
    char line[64];
    snprintf(line, sizeof line, "NEW %5.1f FPS  DSP %5.1f", s.new_fps, s.display_fps);
    c.text(10, 6, line, 115, 255, 196);
    snprintf(line, sizeof line, "FRAME %5.1f ms P95 %5.1f", s.median_ms, s.p95_ms);
    c.text(10, 24, line, 235, 242, 255);
    snprintf(line, sizeof line, "GPU %4.1f ms  AGE %4.1f ms", s.gpu_ms, s.age_ms);
    c.text(10, 42, line, 204, 217, 237);
    char cap[24];
    if (limit)
        snprintf(cap, sizeof cap, "CAP %d", limit);
    else
        snprintf(cap, sizeof cap, "ORIGINAL");
    snprintf(line, sizeof line, "RPT %3.0f%% DROP %llu %s", s.repeat_percent,
             (unsigned long long)s.drops, cap);
    line[26] = 0;
    c.text(10, 60, line, 204, 217, 237);
    if (mode == 2) {
        // Fixed 0..50ms axis from the bottom edge. A red bar means a missed budget.
        const double budget = 1000.0 / (limit ? limit : 60);
        const int base = base_used - 13;
        c.fill(10, base, width - 20, 1, 51, 71, 92, 255);
        c.fill(10, base - int(std::min(50.0, budget) * .7), width - 20, 1, 102, 122, 140, 204);
        for (size_t i = 0; i < s.intervals_ms.size(); ++i) {
            const double ms = s.intervals_ms[i];
            const bool late = ms > budget * 1.2;
            const int bar = int(std::min(50.0, ms) * .7);
            c.fill(10 + int(i * 2.55), base - bar, 2, bar, late ? 255 : 77, late ? 102 : 217,
                   late ? 77 : 255, 255);
        }
    }
    if (capture.enabled) {
        snprintf(line, sizeof line, "CAP C%llu %s", (unsigned long long)capture.id,
                 capture_state::name(capture.state));
        c.text(10, base_used, line, 255, 210, 80);
        snprintf(line, sizeof line, "R%llu P%llu %s", (unsigned long long)capture.render_frame,
                 (unsigned long long)presenter_frame, capture.detailed ? "DETAIL" : "COUNTS");
        c.text(10, base_used + 18, line, 235, 242, 255);
        snprintf(line, sizeof line, "MONO %llu MS",
                 (unsigned long long)(capture.monotonic_ns / 1000000));
        c.text(10, base_used + 36, line, 235, 242, 255);
        c.text(10, base_used + 54, capture_state::reason(capture.reason), 255, 210, 80);
    }
    if (device_ != device || !texture_ || texture_h_ != used) {
        if (device_ && texture_)
            device_->destroy(texture_);
        device_ = device;
        texture_ = device->create_texture(
            {width, used, gpu::Format::RGBA8, gpu::UsageSampled | gpu::UsageCpu, 1});
        texture_h_ = used;
    }
    if (texture_)
        device->upload(texture_, {0, 0, width, used}, pixels_.data(), width * 4);
}

void PerformanceOverlay::draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target,
                              int w, int h, const FramePacingSnapshot &s, double now, int mode,
                              int limit, const CaptureMarker &capture, uint64_t presenter_frame) {
    if (capture.enabled && !mode)
        mode = 1;
    if (!mode || !device || !target || !cb || w <= 0 || h <= 0)
        return;
    if ((capture.enabled && capture.render_frame != last_render_) || !texture_ ||
        mode != last_mode_ || now - refreshed_ >= .25 || device_ != device) {
        update(device, s, mode, limit, capture, presenter_frame);
        last_render_ = capture.render_frame;
        refreshed_ = now;
        last_mode_ = mode;
    }
    if (!texture_)
        return;
    gpu::RenderState state;
    state.color_format[0] = device->describe(target).format;
    state.color_count = 1;
    state.blend_enabled = true;
    state.src_rgb = state.src_alpha = gpu::Blend::One;
    state.dst_rgb = state.dst_alpha = gpu::Blend::OneMinusSrcAlpha;
    gpu::Pipeline pipeline = device->render_pipeline("hud", state);
    if (!pipeline)
        return;
    const double scale = std::clamp(double(h) / 900.0, 1.0, 3.0);
    const double ow = std::min(double(w) - 20, width * scale), oh = texture_h_ * (ow / width);
    float rect[] = {float(1 - 2 * (ow + 10) / w), float(1 - 20.0 / h), float(2 * ow / w),
                    float(-2 * oh / h)};
    gpu::RenderPass pass;
    pass.color_count = 1;
    pass.color[0].texture = target;
    pass.color[0].load = gpu::Load::Load;
    pass.color[0].store = gpu::Store::Store;
    device->begin_render_pass(cb, pass);
    device->set_pipeline(cb, pipeline);
    device->set_bytes(cb, gpu::Stage::Vertex, 0, rect, sizeof rect);
    device->set_texture(cb, gpu::Stage::Fragment, 0, texture_);
    gpu::SamplerState linear;
    linear.mag = linear.min = gpu::Filter::Linear;
    device->set_sampler(cb, gpu::Stage::Fragment, 0, linear);
    device->draw(cb, gpu::Primitive::TriangleStrip, 0, 4);
    device->end_render_pass(cb);
}

// The macOS host defines these (sdl/perf_glass_mac.mm); every other host draws the overlay above.
__attribute__((weak)) bool host_perf_glass_publish(int, const FramePacingSnapshot &, int, double) {
    return false;
}
__attribute__((weak)) void host_perf_glass_tick(void *) {}
