// gpu.h - the device-level GPU interface every host backend implements.
//
// Handles, not native objects, cross this boundary: a GpuTexture is a 64-bit
// id the backend maps to its own object, and that id is also what the
// presenter leases and compares. A texture lives until destroy(); nothing is
// freed by a handle going out of scope, so the owner of a lease is the owner
// of the texture.
//
// One queue. Command buffers committed on a device complete in the order they
// were committed. The renderer and presenter rely on that so a present can
// never run ahead of the scene it shows; a backend that cannot promise it with
// one queue must enforce it with its own synchronisation.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace gpu {

struct Texture {
    uint64_t id = 0;
    explicit operator bool() const {
        return id != 0;
    }
};
struct Buffer {
    uint64_t id = 0;
    explicit operator bool() const {
        return id != 0;
    }
};
struct Pipeline {
    uint64_t id = 0;
    explicit operator bool() const {
        return id != 0;
    }
};
struct CommandBuffer {
    uint64_t id = 0;
    explicit operator bool() const {
        return id != 0;
    }
};
struct Swapchain {
    uint64_t id = 0;
    explicit operator bool() const {
        return id != 0;
    }
};
inline bool operator==(Texture a, Texture b) {
    return a.id == b.id;
}
inline bool operator!=(Texture a, Texture b) {
    return a.id != b.id;
}

enum class Format { BGRA8, RGBA8, R8, Depth32F, RGBA16F }; // RGBA16F: Test86 HDR (linear extended sRGB)
enum Usage : uint32_t {
    UsageSampled = 1,      // read by shaders
    UsageRenderTarget = 2, // written by render passes
    UsageStorage = 4,      // read/written by compute
    UsageCpu = 8,          // uploaded from and read back to the CPU
};
struct TextureDesc {
    int width = 0, height = 0;
    Format format = Format::RGBA8;
    uint32_t usage = UsageSampled;
    int mip_levels = 1; // 1 or the full chain
};
struct Region {
    int x = 0, y = 0, w = 0, h = 0;
};

enum class Blend {
    Zero,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlphaSaturated
};
enum class Compare { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class Cull { None, Front, Back };
enum class Filter { Nearest, Linear };
enum class MipFilter { None, Nearest, Linear };
enum class Address { Repeat, ClampToEdge, MirrorRepeat, ClampToBorder };
enum class Primitive { Points, Lines, Triangles, TriangleStrip };
enum class Load { Load, Clear, DontCare };
enum class Store { Store, DontCare };
enum class Stage { Vertex, Fragment, Compute };

// What a render pipeline needs besides its shader pair. Two colour attachments
// at most: colour and the coverage mask the D3D renderer writes.
struct RenderState {
    Format color_format[2] = {Format::BGRA8, Format::R8};
    int color_count = 1;
    bool depth_attachment = false;
    bool blend_enabled = false;
    Blend src_rgb = Blend::One, dst_rgb = Blend::Zero;
    Blend src_alpha = Blend::One, dst_alpha = Blend::Zero;
    bool write_color = true; // colour write mask for attachment 0 and 1 together
    bool writes_point_size = false;
    uint64_t key() const; // a total order over the fields above, for caches
};
struct DepthState {
    Compare compare = Compare::Always;
    bool write = false;
};
struct SamplerState {
    Address u = Address::ClampToEdge, v = Address::ClampToEdge;
    Filter mag = Filter::Nearest, min = Filter::Nearest;
    MipFilter mip = MipFilter::None;
    int anisotropy = 1;
};

struct ColorAttachment {
    Texture texture;
    Load load = Load::Load;
    Store store = Store::Store;
    float clear[4] = {0, 0, 0, 1};
};
struct DepthAttachment {
    Texture texture;
    Load load = Load::Load;
    Store store = Store::Store;
    float clear = 1.0f;
};
struct RenderPass {
    ColorAttachment color[2];
    int color_count = 1;
    DepthAttachment depth; // texture.id == 0 for none
};
struct Viewport {
    double x, y, w, h, near_z, far_z;
};

// Pending: begun or committed and not yet finished. Unknown ids read as Completed.
enum class CommandStatus { Completed, Error, Pending };

class Device {
  public:
    virtual ~Device() = default;

    // --- textures ---
    virtual Texture create_texture(const TextureDesc &desc) = 0;
    // `pitch` is bytes per row of `bytes`; level 0 unless `level` says otherwise.
    virtual bool upload(Texture t, Region region, const void *bytes, int pitch, int level = 0) = 0;
    // Waits for every committed command that writes `t`, then copies out.
    virtual bool readback(Texture t, Region region, void *bytes, int pitch) = 0;
    virtual void destroy(Texture t) = 0;
    virtual TextureDesc describe(Texture t) = 0; // width 0 for an unknown handle
    // Device bytes the texture occupies, for the HD cache budget.
    virtual uint64_t allocated_bytes(Texture t) = 0;

    // --- buffers ---
    virtual Buffer create_buffer(uint64_t bytes, const void *contents) = 0;
    virtual void update(Buffer b, uint64_t offset, const void *bytes, uint64_t count) = 0;
    // Read back a CPU-visible buffer's contents after the commands writing it completed.
    virtual const void *map_read(Buffer b) = 0;
    virtual uint64_t buffer_bytes(Buffer b) = 0;
    virtual void destroy(Buffer b) = 0;

    // --- pipelines ---
    // `shader` names a vertex/fragment pair in gpu/shaders.md ("d3d", "surface_upload",
    // "compositor", "hud"). The backend caches by (shader, state.key()).
    virtual Pipeline render_pipeline(const std::string &shader, const RenderState &state) = 0;
    // `shader` names a compute kernel ("guest_readback", "guest_readback_fused", "native_brightness").
    virtual Pipeline compute_pipeline(const std::string &shader) = 0;
    // Thread execution width of a compute pipeline, for SIMD-group sized dispatches.
    virtual int thread_execution_width(Pipeline p) = 0;

    // --- command buffers ---
    virtual CommandBuffer begin() = 0;
    virtual void begin_render_pass(CommandBuffer cb, const RenderPass &pass) = 0;
    virtual void set_pipeline(CommandBuffer cb, Pipeline p) = 0;
    virtual void set_depth(CommandBuffer cb, const DepthState &d) = 0;
    virtual void set_cull(CommandBuffer cb, Cull c) = 0;
    virtual void set_viewport(CommandBuffer cb, const Viewport &v) = 0;
    virtual void set_vertex_buffer(CommandBuffer cb, int slot, Buffer b, uint64_t offset) = 0;
    // Inline data, at most 4 KB, copied at call time.
    virtual void set_bytes(CommandBuffer cb, Stage stage, int slot, const void *bytes,
                           uint64_t count) = 0;
    virtual void set_buffer(CommandBuffer cb, Stage stage, int slot, Buffer b, uint64_t offset) = 0;
    virtual void set_texture(CommandBuffer cb, Stage stage, int slot, Texture t) = 0;
    virtual void set_sampler(CommandBuffer cb, Stage stage, int slot, const SamplerState &s) = 0;
    virtual void draw(CommandBuffer cb, Primitive primitive, int first, int count) = 0;
    virtual void end_render_pass(CommandBuffer cb) = 0;

    virtual void begin_compute_pass(CommandBuffer cb) = 0;
    // Threads, not groups: the backend rounds up into `group` sized groups.
    virtual void dispatch_threads(CommandBuffer cb, int tx, int ty, int gx, int gy) = 0;
    virtual void dispatch_groups(CommandBuffer cb, int groups_x, int groups_y, int gx, int gy) = 0;
    virtual void end_compute_pass(CommandBuffer cb) = 0;

    virtual void blit(CommandBuffer cb, Texture src, Region src_region, Texture dst, int dst_x,
                      int dst_y) = 0;
    // Buffer <-> texture copies inside the command stream, `pitch` bytes per row.
    virtual void copy_buffer_to_texture(CommandBuffer cb, Buffer src, uint64_t offset, int pitch,
                                        Texture dst, Region dst_region) = 0;
    virtual void copy_texture_to_buffer(CommandBuffer cb, Texture src, Region src_region,
                                        Buffer dst, uint64_t offset, int pitch) = 0;
    virtual void generate_mipmaps(CommandBuffer cb, Texture t) = 0;
    // An edge-aware enlargement of `src` into `dst` (larger, made with UsageSampled |
    // UsageRenderTarget | UsageStorage), encoded outside any pass: MetalFX's spatial scaler on
    // Metal. False when the backend has none or the textures do not suit it; nothing is encoded.
    virtual bool spatial_upscale(CommandBuffer, Texture /*src*/, Texture /*dst*/) { return false; }

    // Runs on a backend thread after the buffer finishes; `status` says how.
    virtual void on_complete(CommandBuffer cb,
                             std::function<void(CommandStatus, double gpu_ms)> fn) = 0;
    virtual void commit(CommandBuffer cb) = 0;
    virtual void wait(CommandBuffer cb) = 0; // blocks until complete
    virtual CommandStatus status(CommandBuffer cb) = 0;

    // --- swapchain ---
    // `native_surface` is what the window layer hands over: a CAMetalLayer* on
    // macOS. Null on failure.
    virtual Swapchain create_swapchain(void *native_surface, int width, int height) = 0;
    virtual void resize(Swapchain s, int width, int height) = 0;
    virtual Format swapchain_format(Swapchain s) = 0;
    // A texture to render this frame into, or a null handle when none is available.
    // The texture is valid until present() or release_drawable().
    virtual Texture acquire(Swapchain s) = 0;
    virtual void release_drawable(Swapchain s, Texture t) = 0; // acquired but not presented
    // Schedules the present behind `cb`; `presented` runs with the display time
    // in device-clock seconds, or 0 when the display never reported it.
    virtual void present(CommandBuffer cb, Swapchain s, Texture t, double min_duration_seconds,
                         std::function<void(double presented_seconds)> presented) = 0;
    virtual double refresh_period(Swapchain s) = 0; // seconds; 1/60 when unknown
    virtual void destroy(Swapchain s) = 0;
    // Test86: extended-dynamic-range output. Asks the swapchain to present RGBA16F linear extended sRGB
    // (1.0 = the system's SDR white) or to return to BGRA8; the drawables acquired afterwards say which
    // one is in effect (describe().format). False when the backend has no EDR.
    virtual bool set_swapchain_hdr(Swapchain, bool) { return false; }
    // The display's EDR headroom (peak / SDR white): the current value, or the potential one when the
    // display is in an HDR mode; 1 when there is none.
    virtual double edr_headroom(bool potential) {
        (void)potential;
        return 1.0;
    }
    // Test90: the display's own HDR mode (the HDR signal a TV switches on; System Settings > Displays >
    // High Dynamic Range). request 1 asks for it (1|4: and 60 Hz while in HDR, Test91) (asynchronously: the switch can take seconds), 0 puts back a
    // switch this process made, -1 only reports. Returns -1 not available, 0 SDR, 1 HDR, 2 switching.
    virtual int display_hdr_mode(int request) {
        (void)request;
        return -1;
    }
    // How many times this process has switched the display's HDR mode (either way); the presenter reopens
    // the audio output after one, since a TV's HDMI link renegotiates.
    virtual unsigned display_hdr_switches() { return 0; }

    // --- clock ---
    virtual double now_seconds() = 0; // monotonic, the base every callback timestamp uses
};

} // namespace gpu

namespace gpu {
inline uint64_t RenderState::key() const {
    uint64_t k = 0;
    k |= uint64_t(color_format[0]) << 0;
    k |= uint64_t(color_format[1]) << 4;
    k |= uint64_t(color_count) << 8;
    k |= uint64_t(depth_attachment) << 10;
    k |= uint64_t(blend_enabled) << 11;
    k |= uint64_t(src_rgb) << 12;
    k |= uint64_t(dst_rgb) << 16;
    k |= uint64_t(src_alpha) << 20;
    k |= uint64_t(dst_alpha) << 24;
    k |= uint64_t(write_color) << 28;
    k |= uint64_t(writes_point_size) << 29;
    return k;
}
} // namespace gpu
