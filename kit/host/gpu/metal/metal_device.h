// metal_device.h - gpu::Device over Metal. One queue, so command buffers
// complete in commit order without extra synchronisation. Included only by
// Objective-C++ files under gpu/metal/.
#pragma once
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "../gpu.h"

#include <mutex>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace gpu {

class MetalDevice final : public Device {
  public:
    explicit MetalDevice(id<MTLDevice> device);
    ~MetalDevice() override;

    Texture create_texture(const TextureDesc &desc) override;
    bool upload(Texture t, Region region, const void *bytes, int pitch, int level = 0) override;
    bool readback(Texture t, Region region, void *bytes, int pitch) override;
    void destroy(Texture t) override;
    TextureDesc describe(Texture t) override;
    uint64_t allocated_bytes(Texture t) override;

    Buffer create_buffer(uint64_t bytes, const void *contents) override;
    void update(Buffer b, uint64_t offset, const void *bytes, uint64_t count) override;
    const void *map_read(Buffer b) override;
    uint64_t buffer_bytes(Buffer b) override;
    void destroy(Buffer b) override;

    Pipeline render_pipeline(const std::string &shader, const RenderState &state) override;
    Pipeline compute_pipeline(const std::string &shader) override;
    int thread_execution_width(Pipeline p) override;

    CommandBuffer begin() override;
    void begin_render_pass(CommandBuffer cb, const RenderPass &pass) override;
    void set_pipeline(CommandBuffer cb, Pipeline p) override;
    void set_depth(CommandBuffer cb, const DepthState &d) override;
    void set_cull(CommandBuffer cb, Cull c) override;
    void set_viewport(CommandBuffer cb, const Viewport &v) override;
    void set_vertex_buffer(CommandBuffer cb, int slot, Buffer b, uint64_t offset) override;
    void set_bytes(CommandBuffer cb, Stage stage, int slot, const void *bytes,
                   uint64_t count) override;
    void set_buffer(CommandBuffer cb, Stage stage, int slot, Buffer b, uint64_t offset) override;
    void set_texture(CommandBuffer cb, Stage stage, int slot, Texture t) override;
    void set_sampler(CommandBuffer cb, Stage stage, int slot, const SamplerState &s) override;
    void draw(CommandBuffer cb, Primitive primitive, int first, int count) override;
    void end_render_pass(CommandBuffer cb) override;
    void begin_compute_pass(CommandBuffer cb) override;
    void dispatch_threads(CommandBuffer cb, int tx, int ty, int gx, int gy) override;
    void dispatch_groups(CommandBuffer cb, int groups_x, int groups_y, int gx, int gy) override;
    void end_compute_pass(CommandBuffer cb) override;
    void blit(CommandBuffer cb, Texture src, Region src_region, Texture dst, int dst_x,
              int dst_y) override;
    void copy_buffer_to_texture(CommandBuffer cb, Buffer src, uint64_t offset, int pitch,
                                Texture dst, Region dst_region) override;
    void copy_texture_to_buffer(CommandBuffer cb, Texture src, Region src_region, Buffer dst,
                                uint64_t offset, int pitch) override;
    void generate_mipmaps(CommandBuffer cb, Texture t) override;
    bool spatial_upscale(CommandBuffer cb, Texture src, Texture dst) override;
    void on_complete(CommandBuffer cb, std::function<void(CommandStatus, double)> fn) override;
    void commit(CommandBuffer cb) override;
    void wait(CommandBuffer cb) override;
    CommandStatus status(CommandBuffer cb) override;

    Swapchain create_swapchain(void *native_surface, int width, int height) override;
    void resize(Swapchain s, int width, int height) override;
    Format swapchain_format(Swapchain s) override;
    Texture acquire(Swapchain s) override;
    void release_drawable(Swapchain s, Texture t) override;
    void present(CommandBuffer cb, Swapchain s, Texture t, double min_duration_seconds,
                 std::function<void(double)> presented) override;
    double refresh_period(Swapchain s) override;
    void destroy(Swapchain s) override;
    bool set_swapchain_hdr(Swapchain s, bool on) override; // Test86 (metal_surface.mm)
    double edr_headroom(bool potential) override;          // Test86 (metal_surface.mm)
    int display_hdr_mode(int request) override;            // Test90 (metal_surface.mm)
    unsigned display_hdr_switches() override;              // Test90 (metal_surface.mm)
    double now_seconds() override;

    // --- native access for the swapchain ---
    id<MTLDevice> native() const {
        return device_;
    }
    id<MTLCommandQueue> native_queue() const {
        return queue_;
    }
    Texture import_texture(id<MTLTexture> t);
    id<MTLTexture> native_texture(Texture t);

  private:
    struct Tex {
        id<MTLTexture> texture;
        TextureDesc desc;
        bool foreign = false;
    };
    struct Cmd {
        id<MTLCommandBuffer> buffer;
        id<MTLRenderCommandEncoder> render;
        id<MTLComputeCommandEncoder> compute;
        id<MTLBlitCommandEncoder> blit;
        std::vector<std::function<void(CommandStatus, double)>> on_complete;
        unsigned encoder_count = 0;
    };
    struct Chain {
        CAMetalLayer *layer;
        int w = 0, h = 0;
        std::unordered_map<uint64_t, id<CAMetalDrawable>> drawables; // by texture id
    };
    struct Pipe {
        id<MTLRenderPipelineState> render;
        id<MTLComputePipelineState> compute;
    };

    id<MTLLibrary> library_for(const std::string &shader);
    id<MTLFunction> function(const std::string &shader, bool vertex);
    id<MTLBlitCommandEncoder> blit_encoder(Cmd &c);
    void end_encoders(Cmd &c);
    void report_completion(id<MTLCommandBuffer> buffer, uint64_t cb_id, double submitted);
    void wait_for_gpu(); // every committed command, so a CPU read sees its writes
    Cmd *cmd(CommandBuffer cb);

    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLLibrary> compositor_lib_, hud_lib_, d3d_lib_;
    std::mutex mutex_;
    uint64_t next_id_ = 1;
    bool diagnostics_ = false;
    // The MetalFX spatial scaler (id<MTLFXSpatialScaler>), made for one input/output size and
    // format pair (scaler_key_); spatial_upscale remakes it when they change.
    id scaler_ = nil;
    NSUInteger scaler_key_[6] = {};
    bool scaler_unsupported_ = false;
    std::atomic<unsigned> diagnostic_reports_{0};
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint64_t, id<MTLBuffer>> buffers_;
    std::unordered_map<uint64_t, Cmd> commands_;
    std::unordered_map<uint64_t, id<MTLCommandBuffer>> committed_;
    id<MTLCommandBuffer> last_committed_;
    std::unordered_map<uint64_t, Chain> swapchains_;
    std::unordered_map<uint64_t, Pipe> pipelines_;
    std::unordered_map<std::string, std::unordered_map<uint64_t, Pipeline>> render_cache_;
    std::unordered_map<std::string, Pipeline> compute_cache_;
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_cache_;
    std::unordered_map<uint64_t, id<MTLSamplerState>> sampler_cache_;
};

} // namespace gpu
