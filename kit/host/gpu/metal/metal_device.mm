#include "metal_device.h"

#include "shaders_msl.h"
#include "../../../platform/os.h"

#import <CoreVideo/CVHostTime.h>
#import <MetalFX/MetalFX.h>

#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <string.h>

namespace gpu {

namespace {

double diagnostic_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

MTLPixelFormat pixel_format(Format f) {
    switch (f) {
    case Format::BGRA8:
        return MTLPixelFormatBGRA8Unorm;
    case Format::RGBA8:
        return MTLPixelFormatRGBA8Unorm;
    case Format::R8:
        return MTLPixelFormatR8Unorm;
    case Format::Depth32F:
        return MTLPixelFormatDepth32Float;
    case Format::RGBA16F:
        return MTLPixelFormatRGBA16Float;
    }
    return MTLPixelFormatBGRA8Unorm;
}
Format format_of(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatRGBA8Unorm:
        return Format::RGBA8;
    case MTLPixelFormatR8Unorm:
        return Format::R8;
    case MTLPixelFormatDepth32Float:
        return Format::Depth32F;
    case MTLPixelFormatRGBA16Float:
        return Format::RGBA16F;
    default:
        return Format::BGRA8;
    }
}
int bytes_per_pixel(Format f) {
    return f == Format::R8 ? 1 : f == Format::RGBA16F ? 8 : 4;
}
MTLBlendFactor blend(Blend b) {
    switch (b) {
    case Blend::Zero:
        return MTLBlendFactorZero;
    case Blend::One:
        return MTLBlendFactorOne;
    case Blend::SrcAlpha:
        return MTLBlendFactorSourceAlpha;
    case Blend::OneMinusSrcAlpha:
        return MTLBlendFactorOneMinusSourceAlpha;
    case Blend::DstAlpha:
        return MTLBlendFactorDestinationAlpha;
    case Blend::OneMinusDstAlpha:
        return MTLBlendFactorOneMinusDestinationAlpha;
    case Blend::SrcColor:
        return MTLBlendFactorSourceColor;
    case Blend::OneMinusSrcColor:
        return MTLBlendFactorOneMinusSourceColor;
    case Blend::DstColor:
        return MTLBlendFactorDestinationColor;
    case Blend::OneMinusDstColor:
        return MTLBlendFactorOneMinusDestinationColor;
    case Blend::SrcAlphaSaturated:
        return MTLBlendFactorSourceAlphaSaturated;
    }
    return MTLBlendFactorOne;
}
MTLCompareFunction compare(Compare c) {
    switch (c) {
    case Compare::Never:
        return MTLCompareFunctionNever;
    case Compare::Less:
        return MTLCompareFunctionLess;
    case Compare::Equal:
        return MTLCompareFunctionEqual;
    case Compare::LessEqual:
        return MTLCompareFunctionLessEqual;
    case Compare::Greater:
        return MTLCompareFunctionGreater;
    case Compare::NotEqual:
        return MTLCompareFunctionNotEqual;
    case Compare::GreaterEqual:
        return MTLCompareFunctionGreaterEqual;
    case Compare::Always:
        return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}
MTLSamplerAddressMode address(Address a) {
    switch (a) {
    case Address::Repeat:
        return MTLSamplerAddressModeRepeat;
    case Address::ClampToEdge:
        return MTLSamplerAddressModeClampToEdge;
    case Address::MirrorRepeat:
        return MTLSamplerAddressModeMirrorRepeat;
    case Address::ClampToBorder:
        return MTLSamplerAddressModeClampToBorderColor;
    }
    return MTLSamplerAddressModeClampToEdge;
}
MTLLoadAction load(Load l) {
    return l == Load::Clear  ? MTLLoadActionClear
           : l == Load::Load ? MTLLoadActionLoad
                             : MTLLoadActionDontCare;
}
MTLStoreAction store(Store s) {
    return s == Store::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
}
MTLPrimitiveType primitive(Primitive p) {
    switch (p) {
    case Primitive::Points:
        return MTLPrimitiveTypePoint;
    case Primitive::Lines:
        return MTLPrimitiveTypeLine;
    case Primitive::Triangles:
        return MTLPrimitiveTypeTriangle;
    case Primitive::TriangleStrip:
        return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;
}
const char *vertex_name(const std::string &shader) {
    if (shader == "compositor" || shader == "compositor_hdr")
        return "compositor_vertex";
    if (shader == "hud")
        return "hud_v";
    if (shader == "surface_upload")
        return "surface_upload_vertex";
    return "d3d_vertex";
}
const char *fragment_name(const std::string &shader) {
    if (shader == "compositor")
        return "compositor_fragment";
    if (shader == "compositor_hdr")
        return "compositor_hdr_fragment"; // Test86
    if (shader == "hud")
        return "hud_f";
    if (shader == "surface_upload")
        return "surface_upload_fragment";
    return "d3d_fragment";
}
id<MTLLibrary> compile(id<MTLDevice> device, const char *source, const char *what) {
    NSError *error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:source]
                                              options:nil
                                                error:&error];
    if (!lib)
        fprintf(stderr, "[gpu/metal] the %s shaders did not compile: %s\n", what,
                error ? error.localizedDescription.UTF8String : "unknown error");
    return lib;
}

} // namespace

std::unique_ptr<Device> metal_create_device() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        return nullptr;
    return std::make_unique<MetalDevice>(device);
}

MetalDevice::MetalDevice(id<MTLDevice> device) : device_(device), queue_([device newCommandQueue]) {
    const char *diagnostics = recomp_env("METAL_DIAGNOSTICS");
    diagnostics_ = diagnostics && !strcmp(diagnostics, "1");
    if (diagnostics_)
        fprintf(stderr,
                "[gpu/metal] bounded command diagnostics enabled; not a performance baseline\n");
    compositor_lib_ = compile(device, metal::kCompositorSource, "compositor");
    hud_lib_ = compile(device, metal::kHudSource, "hud");
    d3d_lib_ = compile(device, metal::kD3DSource, "Direct3D");
}
MetalDevice::~MetalDevice() {
    wait_for_gpu();
}

id<MTLLibrary> MetalDevice::library_for(const std::string &shader) {
    if (shader == "compositor" || shader == "compositor_hdr")
        return compositor_lib_;
    if (shader == "hud")
        return hud_lib_;
    return d3d_lib_;
}
id<MTLFunction> MetalDevice::function(const std::string &shader, bool vertex) {
    id<MTLLibrary> lib = library_for(shader);
    if (!lib)
        return nil;
    return [lib newFunctionWithName:[NSString stringWithUTF8String:vertex ? vertex_name(shader)
                                                                          : fragment_name(shader)]];
}
MetalDevice::Cmd *MetalDevice::cmd(CommandBuffer cb) {
    auto it = commands_.find(cb.id);
    return it == commands_.end() ? nullptr : &it->second;
}
void MetalDevice::wait_for_gpu() {
    id<MTLCommandBuffer> last;
    {
        std::lock_guard lock(mutex_);
        last = last_committed_;
    }
    if (last)
        [last waitUntilCompleted];
}

// --- textures ---------------------------------------------------------------

Texture MetalDevice::create_texture(const TextureDesc &desc) {
    if (desc.width <= 0 || desc.height <= 0)
        return {};
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format(desc.format)
                                                           width:desc.width
                                                          height:desc.height
                                                       mipmapped:desc.mip_levels > 1];
    MTLTextureUsage usage = 0;
    if (desc.usage & UsageSampled)
        usage |= MTLTextureUsageShaderRead;
    if (desc.usage & UsageRenderTarget)
        usage |= MTLTextureUsageRenderTarget;
    if (desc.usage & UsageStorage)
        usage |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    d.usage = usage ? usage : MTLTextureUsageShaderRead;
    const bool cpu = (desc.usage & UsageCpu) && desc.format != Format::Depth32F;
    d.storageMode = cpu ? MTLStorageModeShared : MTLStorageModePrivate;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:d];
    if (!texture)
        return {};
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    TextureDesc stored = desc;
    stored.mip_levels = int(texture.mipmapLevelCount);
    textures_[id] = Tex{texture, stored, false};
    return {id};
}
Texture MetalDevice::import_texture(id<MTLTexture> t) {
    if (!t)
        return {};
    std::lock_guard lock(mutex_);
    for (auto &[id, tex] : textures_)
        if (tex.texture == t)
            return {id};
    uint64_t id = next_id_++;
    TextureDesc desc;
    desc.width = int(t.width);
    desc.height = int(t.height);
    desc.format = format_of(t.pixelFormat);
    desc.usage =
        UsageSampled | UsageRenderTarget | (t.storageMode == MTLStorageModeShared ? UsageCpu : 0);
    desc.mip_levels = int(t.mipmapLevelCount);
    textures_[id] = Tex{t, desc, true};
    return {id};
}
id<MTLTexture> MetalDevice::native_texture(Texture t) {
    std::lock_guard lock(mutex_);
    auto it = textures_.find(t.id);
    return it == textures_.end() ? nil : it->second.texture;
}
// Every entry point below runs on whichever thread the presenter or the guest
// calls it from, and none of those threads drains an autorelease pool. Metal
// hands out its command buffers, encoders and drawables autoreleased, so a
// pool per call is what lets a drawable's IOSurface go when the frame does;
// without one they piled up at a frame a refresh until the process was paged
// out.
bool MetalDevice::upload(Texture t, Region r, const void *bytes, int pitch, int level) {
    @autoreleasepool {
        id<MTLTexture> texture = native_texture(t);
        if (!texture || !bytes || level < 0 || level >= int(texture.mipmapLevelCount))
            return false;
        const int w = std::max(1, int(texture.width) >> level),
                  h = std::max(1, int(texture.height) >> level);
        if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 || r.x + r.w > w || r.y + r.h > h)
            return false;
        if (texture.storageMode == MTLStorageModeShared) {
            [texture replaceRegion:MTLRegionMake2D(r.x, r.y, r.w, r.h)
                       mipmapLevel:level
                         withBytes:bytes
                       bytesPerRow:pitch];
            return true;
        }
        // Private storage: stage through a shared buffer and a blit.
        const int bpp = bytes_per_pixel(format_of(texture.pixelFormat));
        const NSUInteger row = NSUInteger(r.w) * bpp;
        id<MTLBuffer> staging = [device_ newBufferWithLength:row * r.h
                                                     options:MTLResourceStorageModeShared];
        for (int y = 0; y < r.h; ++y)
            memcpy(static_cast<uint8_t *>(staging.contents) + row * y,
                   static_cast<const uint8_t *>(bytes) + size_t(pitch) * y, row);
        id<MTLCommandBuffer> buffer = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
        [blit copyFromBuffer:staging
                   sourceOffset:0
              sourceBytesPerRow:row
            sourceBytesPerImage:row * r.h
                     sourceSize:MTLSizeMake(r.w, r.h, 1)
                      toTexture:texture
               destinationSlice:0
               destinationLevel:level
              destinationOrigin:MTLOriginMake(r.x, r.y, 0)];
        [blit endEncoding];
        [buffer commit];
        [buffer waitUntilCompleted];
        return true;
    }
}
bool MetalDevice::readback(Texture t, Region r, void *bytes, int pitch) {
    @autoreleasepool {
        id<MTLTexture> texture = native_texture(t);
        if (!texture || !bytes || r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 ||
            r.x + r.w > int(texture.width) || r.y + r.h > int(texture.height))
            return false;
        wait_for_gpu();
        if (texture.storageMode == MTLStorageModeShared) {
            [texture getBytes:bytes
                  bytesPerRow:pitch
                   fromRegion:MTLRegionMake2D(r.x, r.y, r.w, r.h)
                  mipmapLevel:0];
            return true;
        }
        const int bpp = bytes_per_pixel(format_of(texture.pixelFormat));
        const NSUInteger row = NSUInteger(r.w) * bpp;
        id<MTLBuffer> staging = [device_ newBufferWithLength:row * r.h
                                                     options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> buffer = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
        [blit copyFromTexture:texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(r.x, r.y, 0)
                          sourceSize:MTLSizeMake(r.w, r.h, 1)
                            toBuffer:staging
                   destinationOffset:0
              destinationBytesPerRow:row
            destinationBytesPerImage:row * r.h];
        [blit endEncoding];
        [buffer commit];
        [buffer waitUntilCompleted];
        for (int y = 0; y < r.h; ++y)
            memcpy(static_cast<uint8_t *>(bytes) + size_t(pitch) * y,
                   static_cast<const uint8_t *>(staging.contents) + row * y, row);
        return true;
    }
}
void MetalDevice::destroy(Texture t) {
    std::lock_guard lock(mutex_);
    textures_.erase(t.id);
}
TextureDesc MetalDevice::describe(Texture t) {
    std::lock_guard lock(mutex_);
    auto it = textures_.find(t.id);
    return it == textures_.end() ? TextureDesc{} : it->second.desc;
}
uint64_t MetalDevice::allocated_bytes(Texture t) {
    id<MTLTexture> texture = native_texture(t);
    return texture ? uint64_t(texture.allocatedSize) : 0;
}

// --- buffers ----------------------------------------------------------------

Buffer MetalDevice::create_buffer(uint64_t bytes, const void *contents) {
    id<MTLBuffer> buffer = contents ? [device_ newBufferWithBytes:contents
                                                           length:std::max<uint64_t>(bytes, 1)
                                                          options:MTLResourceStorageModeShared]
                                    : [device_ newBufferWithLength:std::max<uint64_t>(bytes, 1)
                                                           options:MTLResourceStorageModeShared];
    if (!buffer)
        return {};
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    buffers_[id] = buffer;
    return {id};
}
void MetalDevice::update(Buffer b, uint64_t offset, const void *bytes, uint64_t count) {
    id<MTLBuffer> buffer;
    {
        std::lock_guard lock(mutex_);
        auto it = buffers_.find(b.id);
        if (it == buffers_.end())
            return;
        buffer = it->second;
    }
    if (offset + count <= buffer.length)
        memcpy(static_cast<uint8_t *>(buffer.contents) + offset, bytes, count);
}
const void *MetalDevice::map_read(Buffer b) {
    id<MTLBuffer> buffer;
    {
        std::lock_guard lock(mutex_);
        auto it = buffers_.find(b.id);
        if (it == buffers_.end())
            return nullptr;
        buffer = it->second;
    }
    wait_for_gpu();
    return buffer.contents;
}
uint64_t MetalDevice::buffer_bytes(Buffer b) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(b.id);
    return it == buffers_.end() ? 0 : it->second.length;
}
void MetalDevice::destroy(Buffer b) {
    std::lock_guard lock(mutex_);
    buffers_.erase(b.id);
}

// --- pipelines --------------------------------------------------------------

Pipeline MetalDevice::render_pipeline(const std::string &shader, const RenderState &state) {
    {
        std::lock_guard lock(mutex_);
        auto &by_key = render_cache_[shader];
        auto it = by_key.find(state.key());
        if (it != by_key.end())
            return it->second;
    }
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = function(shader, true);
    d.fragmentFunction = function(shader, false);
    if (!d.vertexFunction || !d.fragmentFunction)
        return {};
    for (int i = 0; i < state.color_count && i < 2; ++i) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        a.pixelFormat = pixel_format(state.color_format[i]);
        a.writeMask = state.write_color ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        // Attachment 1 is the coverage mask: it never blends.
        a.blendingEnabled = i == 0 && state.blend_enabled;
        if (a.blendingEnabled) {
            a.sourceRGBBlendFactor = blend(state.src_rgb);
            a.destinationRGBBlendFactor = blend(state.dst_rgb);
            a.sourceAlphaBlendFactor = blend(state.src_alpha);
            a.destinationAlphaBlendFactor = blend(state.dst_alpha);
        }
    }
    if (state.depth_attachment)
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError *error = nil;
    id<MTLRenderPipelineState> ps = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
    if (!ps) {
        fprintf(stderr, "[gpu/metal] render pipeline '%s' failed: %s\n", shader.c_str(),
                error ? error.localizedDescription.UTF8String : "unknown error");
        return {};
    }
    std::lock_guard lock(mutex_);
    Pipeline p{next_id_++};
    pipelines_[p.id] = Pipe{ps, nil};
    render_cache_[shader][state.key()] = p;
    return p;
}
Pipeline MetalDevice::compute_pipeline(const std::string &shader) {
    {
        std::lock_guard lock(mutex_);
        auto it = compute_cache_.find(shader);
        if (it != compute_cache_.end())
            return it->second;
    }
    if (!d3d_lib_)
        return {};
    id<MTLFunction> fn =
        [d3d_lib_ newFunctionWithName:[NSString stringWithUTF8String:shader.c_str()]];
    if (!fn)
        return {};
    NSError *error = nil;
    id<MTLComputePipelineState> ps = [device_ newComputePipelineStateWithFunction:fn error:&error];
    if (!ps) {
        fprintf(stderr, "[gpu/metal] compute pipeline '%s' failed: %s\n", shader.c_str(),
                error ? error.localizedDescription.UTF8String : "unknown error");
        return {};
    }
    std::lock_guard lock(mutex_);
    Pipeline p{next_id_++};
    pipelines_[p.id] = Pipe{nil, ps};
    compute_cache_[shader] = p;
    return p;
}
int MetalDevice::thread_execution_width(Pipeline p) {
    std::lock_guard lock(mutex_);
    auto it = pipelines_.find(p.id);
    return it == pipelines_.end() || !it->second.compute
               ? 1
               : int(it->second.compute.threadExecutionWidth);
}

// --- command buffers --------------------------------------------------------

CommandBuffer MetalDevice::begin() {
    @autoreleasepool {
        id<MTLCommandBuffer> buffer = nil;
        if (diagnostics_) {
            if (@available(macOS 11.0, iOS 14.0, *)) {
                MTLCommandBufferDescriptor *d = [MTLCommandBufferDescriptor new];
                d.retainedReferences = YES;
                d.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
                buffer = [queue_ commandBufferWithDescriptor:d];
            }
        }
        if (!buffer)
            buffer = [queue_ commandBuffer];
        std::lock_guard lock(mutex_);
        uint64_t id = next_id_++;
        if (diagnostics_)
            buffer.label = [NSString stringWithFormat:@"native-cb-%llu", (unsigned long long)id];
        Cmd c;
        c.buffer = buffer;
        commands_[id] = std::move(c);
        return {id};
    }
}
void MetalDevice::end_encoders(Cmd &c) {
    if (c.render) {
        [c.render endEncoding];
        c.render = nil;
    }
    if (c.compute) {
        [c.compute endEncoding];
        c.compute = nil;
    }
    if (c.blit) {
        [c.blit endEncoding];
        c.blit = nil;
    }
}
id<MTLBlitCommandEncoder> MetalDevice::blit_encoder(Cmd &c) {
    @autoreleasepool {
        if (!c.blit) {
            end_encoders(c);
            c.blit = [c.buffer blitCommandEncoder];
            if (diagnostics_)
                c.blit.label = [NSString stringWithFormat:@"blit-%u", ++c.encoder_count];
        }
        return c.blit;
    }
}
void MetalDevice::begin_render_pass(CommandBuffer cb, const RenderPass &pass) {
    @autoreleasepool {
        std::lock_guard lock(mutex_);
        Cmd *c = cmd(cb);
        if (!c)
            return;
        end_encoders(*c);
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        for (int i = 0; i < pass.color_count && i < 2; ++i) {
            auto it = textures_.find(pass.color[i].texture.id);
            if (it == textures_.end())
                continue;
            d.colorAttachments[i].texture = it->second.texture;
            d.colorAttachments[i].loadAction = load(pass.color[i].load);
            d.colorAttachments[i].storeAction = store(pass.color[i].store);
            const float *k = pass.color[i].clear;
            d.colorAttachments[i].clearColor = MTLClearColorMake(k[0], k[1], k[2], k[3]);
        }
        if (pass.depth.texture) {
            auto it = textures_.find(pass.depth.texture.id);
            if (it != textures_.end()) {
                d.depthAttachment.texture = it->second.texture;
                d.depthAttachment.loadAction = load(pass.depth.load);
                d.depthAttachment.storeAction = store(pass.depth.store);
                d.depthAttachment.clearDepth = pass.depth.clear;
            }
        }
        c->render = [c->buffer renderCommandEncoderWithDescriptor:d];
        if (diagnostics_)
            c->render.label = [NSString
                stringWithFormat:@"render-%u color=%llu depth=%llu", ++c->encoder_count,
                                 (unsigned long long)(pass.color_count ? pass.color[0].texture.id
                                                                       : 0),
                                 (unsigned long long)pass.depth.texture.id];
        [c->render setFrontFacingWinding:MTLWindingClockwise];
    }
}
void MetalDevice::set_pipeline(CommandBuffer cb, Pipeline p) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto it = pipelines_.find(p.id);
    if (!c || it == pipelines_.end())
        return;
    if (c->render && it->second.render)
        [c->render setRenderPipelineState:it->second.render];
    else if (c->compute && it->second.compute)
        [c->compute setComputePipelineState:it->second.compute];
}
void MetalDevice::set_depth(CommandBuffer cb, const DepthState &ds) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c || !c->render)
        return;
    const uint64_t key = (uint64_t(compare(ds.compare)) << 1) | (ds.write ? 1u : 0u);
    auto it = depth_cache_.find(key);
    if (it == depth_cache_.end()) {
        MTLDepthStencilDescriptor *d = [[MTLDepthStencilDescriptor alloc] init];
        d.depthCompareFunction = compare(ds.compare);
        d.depthWriteEnabled = ds.write;
        it = depth_cache_.emplace(key, [device_ newDepthStencilStateWithDescriptor:d]).first;
    }
    [c->render setDepthStencilState:it->second];
}
void MetalDevice::set_cull(CommandBuffer cb, Cull cull) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c || !c->render)
        return;
    [c->render setCullMode:cull == Cull::None    ? MTLCullModeNone
                           : cull == Cull::Front ? MTLCullModeFront
                                                 : MTLCullModeBack];
}
void MetalDevice::set_viewport(CommandBuffer cb, const Viewport &v) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c || !c->render)
        return;
    [c->render setViewport:MTLViewport{v.x, v.y, v.w, v.h, v.near_z, v.far_z}];
}
void MetalDevice::set_vertex_buffer(CommandBuffer cb, int slot, Buffer b, uint64_t offset) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto it = buffers_.find(b.id);
    if (!c || !c->render || it == buffers_.end())
        return;
    [c->render setVertexBuffer:it->second offset:offset atIndex:slot];
}
void MetalDevice::set_bytes(CommandBuffer cb, Stage stage, int slot, const void *bytes,
                            uint64_t count) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c)
        return;
    if (stage == Stage::Compute) {
        if (c->compute)
            [c->compute setBytes:bytes length:count atIndex:slot];
    } else if (c->render) {
        if (stage == Stage::Vertex)
            [c->render setVertexBytes:bytes length:count atIndex:slot];
        else
            [c->render setFragmentBytes:bytes length:count atIndex:slot];
    }
}
void MetalDevice::set_buffer(CommandBuffer cb, Stage stage, int slot, Buffer b, uint64_t offset) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto it = buffers_.find(b.id);
    if (!c || it == buffers_.end())
        return;
    if (stage == Stage::Compute) {
        if (c->compute)
            [c->compute setBuffer:it->second offset:offset atIndex:slot];
    } else if (c->render) {
        if (stage == Stage::Vertex)
            [c->render setVertexBuffer:it->second offset:offset atIndex:slot];
        else
            [c->render setFragmentBuffer:it->second offset:offset atIndex:slot];
    }
}
void MetalDevice::set_texture(CommandBuffer cb, Stage stage, int slot, Texture t) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto it = textures_.find(t.id);
    if (!c)
        return;
    id<MTLTexture> texture = it == textures_.end() ? nil : it->second.texture;
    if (stage == Stage::Compute) {
        if (c->compute)
            [c->compute setTexture:texture atIndex:slot];
    } else if (c->render) {
        if (stage == Stage::Vertex)
            [c->render setVertexTexture:texture atIndex:slot];
        else
            [c->render setFragmentTexture:texture atIndex:slot];
    }
}
void MetalDevice::set_sampler(CommandBuffer cb, Stage stage, int slot, const SamplerState &s) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c || !c->render)
        return;
    const uint64_t key = uint64_t(s.u) | uint64_t(s.v) << 4 | uint64_t(s.mag) << 8 |
                         uint64_t(s.min) << 12 | uint64_t(s.mip) << 16 |
                         uint64_t(std::clamp(s.anisotropy, 1, 16)) << 20;
    auto it = sampler_cache_.find(key);
    if (it == sampler_cache_.end()) {
        MTLSamplerDescriptor *d = [[MTLSamplerDescriptor alloc] init];
        d.sAddressMode = address(s.u);
        d.tAddressMode = address(s.v);
        d.magFilter =
            s.mag == Filter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.minFilter =
            s.min == Filter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = s.mip == MipFilter::None     ? MTLSamplerMipFilterNotMipmapped
                      : s.mip == MipFilter::Linear ? MTLSamplerMipFilterLinear
                                                   : MTLSamplerMipFilterNearest;
        d.maxAnisotropy = std::clamp(s.anisotropy, 1, 16);
        it = sampler_cache_.emplace(key, [device_ newSamplerStateWithDescriptor:d]).first;
    }
    if (stage == Stage::Vertex)
        [c->render setVertexSamplerState:it->second atIndex:slot];
    else
        [c->render setFragmentSamplerState:it->second atIndex:slot];
}
void MetalDevice::draw(CommandBuffer cb, Primitive p, int first, int count) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c || !c->render || count <= 0)
        return;
    [c->render drawPrimitives:primitive(p) vertexStart:first vertexCount:count];
}
void MetalDevice::end_render_pass(CommandBuffer cb) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (c && c->render) {
        [c->render endEncoding];
        c->render = nil;
    }
}
void MetalDevice::begin_compute_pass(CommandBuffer cb) {
    @autoreleasepool {
        std::lock_guard lock(mutex_);
        Cmd *c = cmd(cb);
        if (!c)
            return;
        end_encoders(*c);
        c->compute = [c->buffer computeCommandEncoder];
        if (diagnostics_)
            c->compute.label = [NSString stringWithFormat:@"compute-%u", ++c->encoder_count];
    }
}
void MetalDevice::dispatch_threads(CommandBuffer cb, int tx, int ty, int gx, int gy) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c || !c->compute || tx <= 0 || ty <= 0)
        return;
    [c->compute dispatchThreads:MTLSizeMake(tx, ty, 1)
          threadsPerThreadgroup:MTLSizeMake(gx, gy, 1)];
}
void MetalDevice::dispatch_groups(CommandBuffer cb, int groups_x, int groups_y, int gx, int gy) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (!c || !c->compute || groups_x <= 0 || groups_y <= 0)
        return;
    [c->compute dispatchThreadgroups:MTLSizeMake(groups_x, groups_y, 1)
               threadsPerThreadgroup:MTLSizeMake(gx, gy, 1)];
}
void MetalDevice::end_compute_pass(CommandBuffer cb) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (c && c->compute) {
        [c->compute endEncoding];
        c->compute = nil;
    }
}
void MetalDevice::blit(CommandBuffer cb, Texture src, Region r, Texture dst, int dst_x, int dst_y) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto s = textures_.find(src.id), d = textures_.find(dst.id);
    if (!c || s == textures_.end() || d == textures_.end() || r.w <= 0 || r.h <= 0)
        return;
    [blit_encoder(*c) copyFromTexture:s->second.texture
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(r.x, r.y, 0)
                           sourceSize:MTLSizeMake(r.w, r.h, 1)
                            toTexture:d->second.texture
                     destinationSlice:0
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(dst_x, dst_y, 0)];
}
// MetalFX's spatial scaler: an edge-aware enlargement of a game frame drawn smaller than the
// screen. Perceptual: the frame's values are gamma-encoded (UNORM), as the game drew them.
bool MetalDevice::spatial_upscale(CommandBuffer cb, Texture src, Texture dst) {
    @autoreleasepool {
        std::lock_guard lock(mutex_);
        Cmd *c = cmd(cb);
        auto s = textures_.find(src.id), d = textures_.find(dst.id);
        if (scaler_unsupported_ || !c || s == textures_.end() || d == textures_.end())
            return false;
        id<MTLTexture> in = s->second.texture, out = d->second.texture;
        const NSUInteger key[6] = {in.width, in.height, out.width, out.height, NSUInteger(in.pixelFormat),
                                   NSUInteger(out.pixelFormat)};
        if (!scaler_ || memcmp(key, scaler_key_, sizeof key) != 0) {
            scaler_ = nil;
            if (![MTLFXSpatialScalerDescriptor supportsDevice:device_]) {
                scaler_unsupported_ = true;
                printf("gpu: MetalFX spatial upscaling is not supported on this GPU\n");
                return false;
            }
            MTLFXSpatialScalerDescriptor *desc = [MTLFXSpatialScalerDescriptor new];
            desc.inputWidth = in.width;
            desc.inputHeight = in.height;
            desc.outputWidth = out.width;
            desc.outputHeight = out.height;
            desc.colorTextureFormat = in.pixelFormat;
            desc.outputTextureFormat = out.pixelFormat;
            desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
            scaler_ = [desc newSpatialScalerWithDevice:device_];
            if (!scaler_)
                return false;
            memcpy(scaler_key_, key, sizeof key);
            printf("gpu: MetalFX spatial upscaling %lux%lu -> %lux%lu\n", (unsigned long)in.width,
                   (unsigned long)in.height, (unsigned long)out.width, (unsigned long)out.height);
            fflush(stdout);
        }
        id<MTLFXSpatialScaler> scaler = scaler_;
        if ((in.usage & scaler.colorTextureUsage) != scaler.colorTextureUsage ||
            (out.usage & scaler.outputTextureUsage) != scaler.outputTextureUsage ||
            out.storageMode != MTLStorageModePrivate)
            return false;
        end_encoders(*c);
        scaler.colorTexture = in;
        scaler.outputTexture = out;
        scaler.inputContentWidth = in.width;
        scaler.inputContentHeight = in.height;
        [scaler encodeToCommandBuffer:c->buffer];
        return true;
    }
}
void MetalDevice::copy_buffer_to_texture(CommandBuffer cb, Buffer src, uint64_t offset, int pitch,
                                         Texture dst, Region r) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto b = buffers_.find(src.id);
    auto d = textures_.find(dst.id);
    if (!c || b == buffers_.end() || d == textures_.end() || r.w <= 0 || r.h <= 0)
        return;
    [blit_encoder(*c) copyFromBuffer:b->second
                        sourceOffset:offset
                   sourceBytesPerRow:pitch
                 sourceBytesPerImage:NSUInteger(pitch) * r.h
                          sourceSize:MTLSizeMake(r.w, r.h, 1)
                           toTexture:d->second.texture
                    destinationSlice:0
                    destinationLevel:0
                   destinationOrigin:MTLOriginMake(r.x, r.y, 0)];
}
void MetalDevice::copy_texture_to_buffer(CommandBuffer cb, Texture src, Region r, Buffer dst,
                                         uint64_t offset, int pitch) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto s = textures_.find(src.id);
    auto b = buffers_.find(dst.id);
    if (!c || s == textures_.end() || b == buffers_.end() || r.w <= 0 || r.h <= 0)
        return;
    [blit_encoder(*c) copyFromTexture:s->second.texture
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(r.x, r.y, 0)
                           sourceSize:MTLSizeMake(r.w, r.h, 1)
                             toBuffer:b->second
                    destinationOffset:offset
               destinationBytesPerRow:pitch
             destinationBytesPerImage:NSUInteger(pitch) * r.h];
}
void MetalDevice::generate_mipmaps(CommandBuffer cb, Texture t) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    auto it = textures_.find(t.id);
    if (!c || it == textures_.end() || it->second.texture.mipmapLevelCount < 2)
        return;
    [blit_encoder(*c) generateMipmapsForTexture:it->second.texture];
}
void MetalDevice::on_complete(CommandBuffer cb, std::function<void(CommandStatus, double)> fn) {
    std::lock_guard lock(mutex_);
    Cmd *c = cmd(cb);
    if (c)
        c->on_complete.push_back(std::move(fn));
}
void MetalDevice::commit(CommandBuffer cb) {
    @autoreleasepool {
        id<MTLCommandBuffer> buffer;
        std::vector<std::function<void(CommandStatus, double)>> callbacks;
        {
            std::lock_guard lock(mutex_);
            Cmd *c = cmd(cb);
            if (!c)
                return;
            end_encoders(*c);
            buffer = c->buffer;
            callbacks.swap(c->on_complete);
            commands_.erase(cb.id);
            committed_[cb.id] = buffer;
            last_committed_ = buffer;
        }
        const uint64_t cb_id = cb.id;
        const double submitted = diagnostics_ ? diagnostic_now() : 0;
        if (!callbacks.empty()) {
            auto shared = std::make_shared<std::vector<std::function<void(CommandStatus, double)>>>(
                std::move(callbacks));
            [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
              if (diagnostics_)
                  report_completion(done, cb_id, submitted);
              const CommandStatus status = done.status == MTLCommandBufferStatusCompleted
                                               ? CommandStatus::Completed
                                               : CommandStatus::Error;
              const double ms = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
              for (auto &fn : *shared)
                  fn(status, ms);
              std::lock_guard lock(mutex_);
              committed_.erase(cb_id);
            }];
        } else {
            [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
              if (diagnostics_)
                  report_completion(done, cb_id, submitted);
              std::lock_guard lock(mutex_);
              committed_.erase(cb_id);
            }];
        }
        [buffer commit];
    }
}
// Capture errors even when nobody polls status(). Reporting is opt-in and
// bounded, after completion, and does not retire targets or alter queue pacing.
void MetalDevice::report_completion(id<MTLCommandBuffer> buffer, uint64_t cb_id, double submitted) {
    const double elapsed = diagnostic_now() - submitted;
    if (buffer.status != MTLCommandBufferStatusError && elapsed < 1.0)
        return;
    if (diagnostic_reports_.fetch_add(1) >= 32)
        return;
    fprintf(stderr,
            "[gpu/metal/diagnostic] cb=%llu status=%lu commit_to_callback_ms=%.3f "
            "gpu_ms=%.3f kernel_ms=%.3f error_domain=%s error_code=%ld error=%s\n",
            (unsigned long long)cb_id, (unsigned long)buffer.status, elapsed * 1000.0,
            (buffer.GPUEndTime - buffer.GPUStartTime) * 1000.0,
            (buffer.kernelEndTime - buffer.kernelStartTime) * 1000.0,
            buffer.error ? buffer.error.domain.UTF8String : "none", (long)buffer.error.code,
            buffer.error ? buffer.error.localizedDescription.UTF8String : "none");
    if (@available(macOS 11.0, iOS 14.0, *)) {
        NSArray<id<MTLCommandBufferEncoderInfo>> *encoders =
            buffer.error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
        unsigned count = 0;
        for (id<MTLCommandBufferEncoderInfo> encoder in encoders) {
            if (count++ == 64)
                break;
            fprintf(stderr, "[gpu/metal/diagnostic] cb=%llu encoder=%s state=%ld\n",
                    (unsigned long long)cb_id, encoder.label.UTF8String ?: "unlabeled",
                    (long)encoder.errorState);
        }
    }
}
void MetalDevice::wait(CommandBuffer cb) {
    id<MTLCommandBuffer> buffer;
    {
        std::lock_guard lock(mutex_);
        auto it = committed_.find(cb.id);
        if (it == committed_.end())
            return;
        buffer = it->second;
    }
    [buffer waitUntilCompleted];
}
CommandStatus MetalDevice::status(CommandBuffer cb) {
    id<MTLCommandBuffer> buffer;
    {
        std::lock_guard lock(mutex_);
        if (commands_.count(cb.id))
            return CommandStatus::Pending;
        auto it = committed_.find(cb.id);
        if (it == committed_.end())
            return CommandStatus::Completed;
        buffer = it->second;
    }
    switch (buffer.status) {
    case MTLCommandBufferStatusError: {
        static bool reported = false;
        if (!reported) {
            reported = true;
            fprintf(stderr, "[gpu] a command buffer failed: %s\n",
                    buffer.error ? buffer.error.localizedDescription.UTF8String
                                 : "no error object");
        }
        return CommandStatus::Error;
    }
    case MTLCommandBufferStatusCompleted:
        return CommandStatus::Completed;
    default:
        return CommandStatus::Pending;
    }
}

// --- swapchain (see metal_surface.mm for the layer side) --------------------

Swapchain MetalDevice::create_swapchain(void *native_surface, int width, int height) {
    if (!native_surface || width <= 0 || height <= 0)
        return {};
    CAMetalLayer *layer = (__bridge CAMetalLayer *)native_surface;
    layer.device = device_;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.maximumDrawableCount = 3;
    // Immediate presentation must also opt out of CAMetalLayer's default
    // display synchronization. The selector exists on macOS only; capability
    // detection keeps the shared Metal backend usable on mobile platforms.
    if ([layer respondsToSelector:NSSelectorFromString(@"setDisplaySyncEnabled:")]) {
        const char *mode = recomp_env("HOST_PRESENT_PACING");
        const bool immediate = mode && strcmp(mode, "immediate") == 0;
        [layer setValue:@(!immediate) forKey:@"displaySyncEnabled"];
        fprintf(stderr, "[gpu] Metal presentation: display synchronization %s\n",
                [[layer valueForKey:@"displaySyncEnabled"] boolValue] ? "ON" : "OFF");
    }
    layer.drawableSize = CGSizeMake(width, height);
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    Chain chain;
    chain.layer = layer;
    chain.w = width;
    chain.h = height;
    swapchains_[id] = std::move(chain);
    return {id};
}
void MetalDevice::resize(Swapchain s, int width, int height) {
    CAMetalLayer *layer;
    {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(s.id);
        if (it == swapchains_.end() || width <= 0 || height <= 0)
            return;
        it->second.w = width;
        it->second.h = height;
        layer = it->second.layer;
    }
    layer.drawableSize = CGSizeMake(width, height);
}
Format MetalDevice::swapchain_format(Swapchain s) {
    // Test86: what the layer presents now (BGRA8, or RGBA16F while HDR output is on).
    CAMetalLayer *layer = nil;
    {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(s.id);
        if (it != swapchains_.end())
            layer = it->second.layer;
    }
    return layer && layer.pixelFormat == MTLPixelFormatRGBA16Float ? Format::RGBA16F : Format::BGRA8;
}
Texture MetalDevice::acquire(Swapchain s) {
    @autoreleasepool {
        CAMetalLayer *layer;
        {
            std::lock_guard lock(mutex_);
            auto it = swapchains_.find(s.id);
            if (it == swapchains_.end())
                return {};
            layer = it->second.layer;
        }
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable)
            return {};
        Texture t = import_texture(drawable.texture);
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(s.id);
        if (it != swapchains_.end())
            it->second.drawables[t.id] = drawable;
        return t;
    }
}
void MetalDevice::release_drawable(Swapchain s, Texture t) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it != swapchains_.end())
        it->second.drawables.erase(t.id);
    textures_.erase(t.id);
}
void MetalDevice::present(CommandBuffer cb, Swapchain s, Texture t, double min_duration,
                          std::function<void(double)> presented) {
    @autoreleasepool {
        id<CAMetalDrawable> drawable;
        id<MTLCommandBuffer> buffer;
        {
            std::lock_guard lock(mutex_);
            auto chain = swapchains_.find(s.id);
            Cmd *c = cmd(cb);
            if (chain == swapchains_.end() || !c)
                return;
            auto d = chain->second.drawables.find(t.id);
            if (d == chain->second.drawables.end())
                return;
            drawable = d->second;
            chain->second.drawables.erase(d);
            textures_.erase(t.id);
            end_encoders(*c);
            buffer = c->buffer;
        }
        if (presented) {
            auto fn = std::make_shared<std::function<void(double)>>(std::move(presented));
            [drawable addPresentedHandler:^(id<MTLDrawable> d) {
              (*fn)(d.presentedTime);
            }];
        }
        if (min_duration > 0)
            [buffer presentDrawable:drawable afterMinimumDuration:min_duration];
        else
            [buffer presentDrawable:drawable];
    }
}
void MetalDevice::destroy(Swapchain s) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it == swapchains_.end())
        return;
    for (auto &[tex_id, drawable] : it->second.drawables)
        textures_.erase(tex_id);
    swapchains_.erase(it);
}
double MetalDevice::now_seconds() {
    return double(CVGetCurrentHostTime()) / CVGetHostClockFrequency();
}

} // namespace gpu
