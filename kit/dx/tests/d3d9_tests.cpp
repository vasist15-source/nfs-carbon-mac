// Small, synthetic effects exercise the real guest ABI and CTAB binding.
// No game files, window, physical controller or sound device are involved.
#include <sys/resource.h>
#include "../dx.h"
#include "../d3d9_pipeline.h"
#include "../host_d9.h"
#include "../dds_volume.h"
#include "../../runtime/memory.h"
#include "guest_abi.h"

#include <array>
#include <string>
#include <unordered_map>
#include <algorithm>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(c)) {                                                                                \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
        }                                                                                          \
    } while (0)

static HostD9Draw captured;
static void test_volume_dds() {
    std::vector<uint8_t> b(128 + (4 * 2 * 8 + 2 * 1 * 4 + 1 * 1 * 2 + 1) * 4, 0);
    auto put = [&](unsigned word, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            b[word * 4 + i] = uint8_t(value >> (8 * i));
    };
    const uint32_t header[32] = {0x20534444, 124,  0x821007,   2,        4,        0, 8,  4,
                                 0,          0,    0,          0,        0,        0, 0,  0,
                                 0,          0,    0,          32,       0x41,     0, 32, 0xff0000,
                                 0xff00,     0xff, 0xff000000, 0x40100a, 0x200000, 0, 0,  0};
    for (unsigned i = 0; i < 32; ++i)
        put(i, header[i]);
    for (size_t i = 128; i < b.size(); ++i)
        b[i] = uint8_t(i * 17);
    ddsvolume::View v;
    CHECK(ddsvolume::parse(b.data(), b.size(), v));
    CHECK(v.width == 4 && v.height == 2 && v.depth == 8 && v.levels == 4 && v.format == 21);
    CHECK(v.mip[1].width == 2 && v.mip[1].height == 1 && v.mip[1].depth == 4);
    CHECK(v.mip[3].offset == b.size() - 4 && v.mip[3].bytes == 4);
    CHECK(v.mip[0].row_pitch == 16 && v.mip[0].slice_pitch == 32);
    CHECK(!ddsvolume::parse(nullptr, b.size(), v));
    for (size_t n = 0; n < b.size(); ++n) {
        CHECK(!ddsvolume::parse(b.data(), n, v));
        CHECK(v.levels == 0 && v.mip[0].bytes == 0);
    }
    b.push_back(0);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
    b.pop_back();
    for (auto [word, value] : std::initializer_list<std::pair<unsigned, uint32_t>>{
             {0, 0},   {1, 123},     {2, 0x21007},   {2, 0x821007 | 0x80000},
             {3, 0},   {4, 513},     {6, 0},         {7, 0},
             {7, 5},   {19, 31},     {20, 4},        {21, 0x30315844},
             {22, 24}, {23, 0xff},   {24, 0xff},     {25, 0xff0000},
             {26, 0},  {27, 0x1008}, {27, 0x400008}, {28, 0x200200},
             {29, 1},  {30, 1},      {31, 1},        {5, 16}}) {
        put(word, value);
        CHECK(!ddsvolume::parse(b.data(), b.size(), v));
        CHECK(v.width == 0 && v.mip[0].bytes == 0);
        put(word, header[word]);
    }
    put(2, header[2] | 8);
    put(5, 16);
    CHECK(ddsvolume::parse(b.data(), b.size(), v));
    put(5, 20); // unsupported padded rows cannot be silently read as packed
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
    put(2, header[2]);
    put(5, 0);
    put(20, 0x40);
    put(26, 0);
    CHECK(ddsvolume::parse(b.data(), b.size(), v) && v.format == 22);
    put(3, 512);
    put(4, 512);
    put(6, 512);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v)); // reject budget before pointer arithmetic
    put(3, 0xffffffff);
    put(4, 0xffffffff);
    put(6, 0xffffffff);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
    // A valid single-mip non-cubic texture without mip-count metadata.
    b.resize(128 + 3 * 1 * 5 * 4);
    put(2, 0x801007);
    put(3, 1);
    put(4, 3);
    put(6, 5);
    put(7, 0);
    put(27, 0x1008);
    CHECK(ddsvolume::parse(b.data(), b.size(), v) && v.levels == 1);
    put(7, 2);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
}
static unsigned draws = 0, rejected_draws = 0;
static bool admission_enabled = false;
static HostD9Draw rejected;
extern "C" void host_d9_admission_entry(uint64_t) {}
extern "C" int host_d9_admission_enabled(void) {
    return admission_enabled;
}
extern "C" void host_d9_rejected_draw(const HostD9Draw *d) {
    rejected = *d;
    ++rejected_draws;
}
extern "C" int host_d9_active(void) {
    return 1;
}
extern "C" void host_d9_draw(const HostD9Draw *d) {
    captured = *d;
    ++draws;
}

struct VolumeUpload {
    uint32_t id, slice, level, pitch;
    std::vector<uint8_t> data;
};
static std::unordered_map<uint32_t, HostD9TextureDesc> defined_textures;
static std::vector<VolumeUpload> volume_uploads;
static std::vector<uint32_t> dropped_textures;
extern "C" void host_d9_texture_define(const HostD9TextureDesc *d) {
    defined_textures[d->id] = *d;
}
extern "C" void host_d9_texture_upload(uint32_t id, uint32_t slice, uint32_t level,
                                       const uint8_t *bytes, uint32_t pitch) {
    const auto it = defined_textures.find(id);
    if (it == defined_textures.end() || it->second.kind != HOST_D9_TEX_VOLUME)
        return;
    const uint32_t h = std::max(it->second.height >> level, 1u);
    const auto *p = static_cast<const uint8_t *>(bytes);
    volume_uploads.push_back({id, slice, level, pitch, {p, p + pitch * h}});
}
extern "C" void host_d9_texture_drop(uint32_t id) {
    dropped_textures.push_back(id);
    defined_textures.erase(id);
}
static uint32_t make_volume(uint32_t dev, uint32_t pool = 1, uint32_t fmt = 21, uint32_t usage = 0,
                            uint32_t w = 2, uint32_t h = 2, uint32_t d = 2, uint32_t levels = 0) {
    CHECK(call_method(dev, 24, {w, h, d, levels, usage, fmt, pool, sc(0x600), 0}) == 0);
    CHECK(rd32(sc(0x600)) != 0);
    return rd32(sc(0x600));
}
static void test_volume(uint32_t dev) {
    constexpr uint32_t invalid = 0x8876086c;
    const uint32_t tex = make_volume(dev);
    const uint32_t id = com_this(tex)->id;
    CHECK(call_method(tex, 10) == 4); // D3DRTYPE_VOLUMETEXTURE
    CHECK(call_method(tex, 13) == 2);
    CHECK(call_method(tex, 17, {0, sc(0x620)}) == 0);
    const uint32_t expected[] = {21, 2, 0, 1, 2, 2, 2};
    CHECK(!memcmp(gm_ptr(sc(0x620)), expected, sizeof expected));
    CHECK(call_method(tex, 17, {1, sc(0x620)}) == 0);
    CHECK(rd32(sc(0x630)) == 1 && rd32(sc(0x634)) == 1 && rd32(sc(0x638)) == 1);
    CHECK(call_method(tex, 19, {0, sc(0x660), 0, 0}) == 0);
    CHECK(rd32(sc(0x660)) == 8 && rd32(sc(0x664)) == 16);
    const uint32_t data = rd32(sc(0x668));
    for (uint32_t i = 0; i < 32; ++i)
        gm_ptr(data)[i] = uint8_t(i);
    CHECK(call_method(tex, 19, {0, sc(0x680), 0, 0}) == invalid); // no nested lock
    CHECK(call_method(tex, 20, {0}) == 0);
    CHECK(call_method(tex, 20, {0}) == invalid);
    volume_uploads.clear();
    call_method(tex, 9); // PreLoad uploads canonical storage
    CHECK(defined_textures.at(id).depth == 2 && defined_textures.at(id).levels == 2);
    CHECK(volume_uploads.size() == 3);
    for (uint32_t z = 0; z < 2; ++z) {
        const auto &u = volume_uploads[z];
        CHECK(u.id == id && u.slice == z && u.level == 0 && u.pitch == 8 && u.data.size() == 16);
        for (uint32_t i = 0; i < 16; ++i)
            CHECK(u.data[i] == uint8_t(z * 16 + i));
    }
    volume_uploads.clear();
    call_method(tex, 9);
    CHECK(volume_uploads.empty()); // no duplicate unchanged upload
    // A non-origin sub-box returns full pitches and an offset into the same mip.
    const uint32_t box[] = {1, 1, 2, 2, 1, 2};
    memcpy(gm_ptr(sc(0x700)), box, sizeof box);
    CHECK(call_method(tex, 19, {0, sc(0x660), sc(0x700), 0x8000}) == 0);
    CHECK(rd32(sc(0x660)) == 8 && rd32(sc(0x664)) == 16);
    CHECK(rd32(rd32(sc(0x668))) == 0x1f1e1d1c);
    wr32(rd32(sc(0x668)), 0xabcdef01);
    CHECK(call_method(tex, 20, {0}) == 0);
    call_method(tex, 9);
    CHECK(volume_uploads.empty()); // NO_DIRTY_UPDATE defers publication
    CHECK(call_method(tex, 21, {sc(0x700)}) == 0);
    call_method(tex, 9);
    CHECK(volume_uploads.size() == 3);
    CHECK(volume_uploads[1].data[12] == 1 && volume_uploads[1].data[15] == 0xab);
    volume_uploads.clear();
    CHECK(call_method(tex, 19, {0, sc(0x660), sc(0x700), 0x10}) == 0);
    wr32(rd32(sc(0x668)), 0); // illegal guest write through READONLY cannot publish
    CHECK(call_method(tex, 20, {0}) == 0);
    CHECK(call_method(tex, 21, {0}) == 0);
    call_method(tex, 9);
    CHECK(volume_uploads[1].data[15] == 0xab);
    // Fail before writing outputs or mutating storage for bad extents/pointers/flags.
    for (uint32_t level : {2u, 0xffffffffu}) {
        wr32(sc(0x660), 0xdeadbeef);
        CHECK(call_method(tex, 17, {level, sc(0x660)}) == invalid);
        CHECK(rd32(sc(0x660)) == 0xdeadbeef);
        CHECK(call_method(tex, 19, {level, sc(0x660), 0, 0}) == invalid);
        CHECK(rd32(sc(0x660)) == 0xdeadbeef);
    }
    for (uint32_t off : {0u, 4u, 8u, 12u, 16u, 20u}) {
        memcpy(gm_ptr(sc(0x700)), box, sizeof box);
        wr32(sc(0x700) + off, 0xffffffffu);
        CHECK(call_method(tex, 19, {0, sc(0x660), sc(0x700), 0}) == invalid);
    }
    for (uint32_t ptr : {0u, 0xfffffffcu}) {
        CHECK(call_method(tex, 19, {0, ptr, 0, 0}) == invalid);
        CHECK(call_method(tex, 17, {0, ptr}) == invalid);
        CHECK(call_method(tex, 18, {0, ptr}) == invalid);
    }
    CHECK(call_method(tex, 19, {0, sc(0x660), 0xfffffffcu, 0}) == invalid);
    for (uint32_t flags : {0x1000u, 0x2000u, 0x2010u, 0xffffffffu})
        CHECK(call_method(tex, 19, {0, sc(0x660), 0, flags}) == invalid);
    CHECK(call_method(tex, 11, {99}) == 0);
    CHECK(call_method(tex, 12) == 1);
    CHECK(call_method(tex, 11, {0}) == 1);
    CHECK(call_method(tex, 7, {19}) == 0 && call_method(tex, 8) == 19);
    // Prefix interface identity, a real mip interface and parent lifetime.
    const uint32_t base_iid[] = {0x580ca87e, 0x4d541d3c, 0xd3b71d99, 0xce98c2e3};
    const uint32_t tex_iid[] = {0x2518526c, 0x4111e789, 0xef47b9a7, 0xe6138d32};
    const uint32_t vol_iid[] = {0x24f416e6, 0x4aa71f67, 0x3fd38eb8, 0xa128316f};
    memcpy(gm_ptr(sc(0x740)), base_iid, 16);
    CHECK(call_method(tex, 0, {sc(0x740), sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == tex);
    CHECK(call_method(tex, 2) == 1);
    CHECK(call_method(tex, 18, {0, sc(0x600)}) == 0);
    const uint32_t mip = rd32(sc(0x600)), mip_id = com_this(mip)->id;
    CHECK(com_this(tex)->refs == 2);
    CHECK(call_method(mip, 8, {sc(0x620)}) == 0);
    CHECK(!memcmp(gm_ptr(sc(0x620)), expected, sizeof expected));
    memcpy(gm_ptr(sc(0x740)), vol_iid, 16);
    CHECK(call_method(mip, 0, {sc(0x740), sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == mip);
    CHECK(call_method(mip, 2) == 1);
    memcpy(gm_ptr(sc(0x740)), tex_iid, 16);
    CHECK(call_method(mip, 7, {sc(0x740), sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == tex);
    CHECK(call_method(tex, 2) == 2);
    memset(gm_ptr(sc(0x740)), 0xaa, 16);
    CHECK(call_method(mip, 7, {sc(0x740), sc(0x600)}) == 0x80004002u);
    CHECK(rd32(sc(0x600)) == 0);
    CHECK(call_method(tex, 2) == 1); // external mip alone now keeps the resource alive
    CHECK(call_method(mip, 9, {sc(0x660), 0, 0x10}) == 0);
    CHECK(gm_ptr(rd32(sc(0x668)))[31] == 0xab);
    CHECK(call_method(mip, 10) == 0);
    CHECK(call_method(mip, 2) == 0);
    CHECK(!com_get(id) && !com_get(mip_id));
    CHECK(!defined_textures.count(id));
    // The longest dimension determines mip count, even when it is depth.
    const uint32_t deep = make_volume(dev, 1, 28, 0, 1, 1, 4);
    CHECK(call_method(deep, 13) == 3);
    CHECK(call_method(deep, 19, {2, sc(0x660), 0, 0}) == 0);
    CHECK(rd32(sc(0x660)) == 1 && rd32(sc(0x664)) == 1);
    CHECK(call_method(deep, 20, {2}) == 0);
    call_method(deep, 2);
    for (const auto dims : {std::array<uint32_t, 3>{0, 2, 2},
                            {2, 0, 2},
                            {2, 2, 0},
                            {513, 2, 2},
                            {2, 513, 2},
                            {2, 2, 513},
                            {512, 512, 512},
                            {0xffffffffu, 0xffffffffu, 0xffffffffu}}) {
        wr32(sc(0x600), 0xdeadbeef);
        CHECK(call_method(dev, 24, {dims[0], dims[1], dims[2], 0, 0, 21, 1, sc(0x600), 0}) ==
              invalid);
        CHECK(rd32(sc(0x600)) == 0);
    }
    for (const auto args : {std::array<uint32_t, 4>{3, 0, 21, 1},
                            {0, 1, 21, 1},
                            {0, 2, 21, 1},
                            {0, 0x400, 21, 1},
                            {0, 0x200, 21, 1},
                            {0, 0, 75, 1},
                            {0, 0, 0x31545844, 1},
                            {0, 0, 21, 4}}) {
        CHECK(call_method(dev, 24, {2, 2, 2, args[0], args[1], args[2], args[3], sc(0x600), 0}) ==
              invalid);
        CHECK(rd32(sc(0x600)) == 0);
    }
    // Default-pool dynamic locks and system-memory to GPU updates are distinct paths.
    const uint32_t dyn = make_volume(dev, 0, 22, 0x200);
    CHECK(call_method(dyn, 19, {0, sc(0x660), 0, 0x2000}) == 0);
    CHECK(call_method(dyn, 20, {0}) == 0);
    call_method(dyn, 2);
    const uint32_t src = make_volume(dev, 2), dst = make_volume(dev, 0);
    const uint32_t dst_id = com_this(dst)->id;
    CHECK(call_method(dst, 19, {0, sc(0x660), 0, 0}) == invalid);
    CHECK(call_method(dev, 65, {0, src}) == invalid); // system-memory resources cannot be sampled
    CHECK(call_method(src, 19, {0, sc(0x660), 0, 0}) == 0);
    memset(gm_ptr(rd32(sc(0x668))), 0x71, 32);
    CHECK(call_method(dev, 31, {src, dst}) == invalid); // locked resource, no partial update
    CHECK(call_method(src, 20, {0}) == 0);
    CHECK(call_method(dev, 31, {src, dst}) == 0);
    volume_uploads.clear();
    call_method(dst, 9);
    CHECK(volume_uploads.size() == 3 && volume_uploads[0].data[0] == 0x71);
    CHECK(call_method(dev, 65, {0, dst}) == 0);
    const uint8_t decl[] = {0, 0, 0, 0, 2, 0, 0, 0, 0xff, 0, 0, 0, 17, 0, 0, 0};
    memcpy(gm_ptr(sc(0x300)), decl, sizeof decl);
    CHECK(call_method(dev, 86, {sc(0x300), sc(0x600)}) == 0);
    const uint32_t declaration = rd32(sc(0x600));
    CHECK(call_method(dev, 87, {declaration}) == 0);
    const float tri[] = {-1, 1, .5f, 3, 1, .5f, -1, -3, .5f};
    memcpy(gm_ptr(sc(0x400)), tri, sizeof tri);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(captured.sampler_texture[0] == dst_id);
    call_method(declaration, 2);
    CHECK(call_method(dev, 64, {0, sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == dst);
    call_method(dst, 2);             // returned GetTexture reference
    CHECK(call_method(dst, 2) == 1); // device binding alone retains it
    CHECK(com_get(dst_id));
    CHECK(call_method(dev, 65, {0, 0}) == 0);
    CHECK(!com_get(dst_id));
    call_method(src, 2);
}

struct Bytes {
    std::vector<uint8_t> data;
    uint32_t words(std::initializer_list<uint32_t> list) {
        const uint32_t start = uint32_t(data.size());
        for (uint32_t x : list)
            for (unsigned k = 0; k < 4; ++k)
                data.push_back(uint8_t(x >> (k * 8)));
        return start;
    }
    uint32_t raw(const std::vector<uint8_t> &b) {
        const uint32_t start = uint32_t(data.size());
        data.insert(data.end(), b.begin(), b.end());
        while (data.size() % 4)
            data.push_back(0);
        return start;
    }
    uint32_t text(const char *s) {
        uint32_t start = words({uint32_t(strlen(s) + 1)});
        raw(std::vector<uint8_t>(s, s + strlen(s) + 1));
        return start;
    }
};

// Hand-written CTAB: one BOOL parameter, independent register index in each
// shader. These bytes are a test fixture, not extracted proprietary assets.
static std::vector<uint8_t> shader(bool vertex, const char *name, uint32_t index, uint32_t count) {
    const uint32_t version = vertex ? 0xfffe0200u : 0xffff0200u;
    Bytes table;
    table.words({28, 0, version, 1, 28, 0, 0});
    table.words({64, index << 16, count, 48, 0});
    table.words({1u << 16, 0x00010001u, count, 0});
    table.raw(std::vector<uint8_t>(name, name + strlen(name) + 1));
    Bytes out;
    out.words({version, 0xfffeu | uint32_t((table.data.size() + 4) / 4) << 16, 0x42415443u});
    out.raw(table.data);
    if (vertex)
        out.words({0x0200001fu, 0x80000000u, 0x900f0000u, 0x02000001u, 0xc00f0000u, 0x90e40000u});
    else
        out.words({0x02000001u, 0x800f0800u, 0xa0e40000u});
    out.words({0xffffu});
    return out.data;
}

static std::vector<uint8_t> effect() {
    Bytes b;
    b.words({0}); // offset 0 is the null name
    const uint32_t sn = b.text("Switch"), mn = b.text("Many"), tn = b.text("Main"),
                   pn = b.text("P0");
    const uint32_t st = b.words({1, 0, sn, 0, 0, 1, 1});
    const uint32_t mt = b.words({1, 0, mn, 0, 2, 1, 1});
    const uint32_t sv = b.words({0}), mv = b.words({0, 1});
    const uint32_t vt = b.words({16, 4, 0, 0, 0}), pt = b.words({15, 4, 0, 0, 0});
    const uint32_t vi = b.words({1}), pi = b.words({2});
    const uint32_t start = b.words({2, 1, 0, 2});
    b.words({st, sv, 0, 0, mt, mv, 0, 0});
    b.words({tn, 0, 1, pn, 0, 2, 146, 0, vt, vi, 147, 0, pt, pi});
    b.words({2, 0}); // two shader objects, no extra resources
    for (bool vertex : {true, false}) {
        auto code = shader(vertex, vertex ? "Switch" : "Many", vertex ? 15 : 0, vertex ? 1 : 2);
        b.words({vertex ? 1u : 2u, uint32_t(code.size())});
        b.raw(code);
    }
    Bytes out;
    out.words({0xfeff0901u, start});
    out.raw(b.data);
    return out.data;
}

// Bounded readback (GetRenderTargetData + LockRect DONOTWAIT) against a scripted renderer.
static int rb_ready = 0, rb_sync_reads = 0, rb_forgets = 0, rb_requests = 0;
static uint32_t rb_ticket = 0, rb_source = 0;
extern "C" void host_d9_texture_read_async(uint32_t id, uint32_t, uint32_t, uint32_t ticket) {
    rb_source = id;
    rb_ticket = ticket;
    rb_ready = 0;
    ++rb_requests;
}
extern "C" int host_d9_texture_read_poll(uint32_t ticket, uint8_t *b, uint32_t pitch, uint32_t w,
                                         uint32_t h) {
    if (ticket != rb_ticket)
        return -1;
    if (!rb_ready)
        return 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t v = 0x11000000u + y * 16 + x;
            memcpy(b + (size_t)y * pitch + x * 4, &v, 4);
        }
    return 1;
}
extern "C" void host_d9_texture_read_forget(uint32_t) {
    ++rb_forgets;
}
extern "C" int host_d9_texture_read(uint32_t, uint32_t, uint32_t, uint8_t *b, uint32_t pitch) {
    ++rb_sync_reads;
    memset(b, 0x55, (size_t)pitch * 4);
    return 1;
}
static void test_state_getters_and_readback(uint32_t dev) {
    constexpr uint32_t invalid = 0x8876086c, still_drawing = 0x8876021c, not_found = 0x88760866;
    // Render and sampler states read back what was set, and the documented defaults otherwise.
    CHECK(call_method(dev, 57, {27, 1}) == 0);
    CHECK(call_method(dev, 58, {27, sc(0x10)}) == 0 && rd32(sc(0x10)) == 1);
    CHECK(call_method(dev, 58, {22, sc(0x10)}) == 0 && rd32(sc(0x10)) == 3); // CULLMODE ccw
    CHECK(call_method(dev, 58, {300, sc(0x10)}) == invalid);
    CHECK(call_method(dev, 69, {0, 5, 2}) == 0);
    CHECK(call_method(dev, 68, {0, 5, sc(0x10)}) == 0 && rd32(sc(0x10)) == 2);
    CHECK(call_method(dev, 68, {16, 5, sc(0x10)}) == invalid);
    // Render targets: a referenced surface for 0, NOTFOUND for an unset one.
    CHECK(call_method(dev, 38, {0, sc(0x1c)}) == 0);
    const uint32_t original_target = rd32(sc(0x1c));
    CHECK(original_target != 0);
    CHECK(call_method(dev, 28, {4, 4, 21, 0, 0, 0, sc(0x20), 0}) == 0);
    const uint32_t rt = rd32(sc(0x20));
    CHECK(rt != 0);
    CHECK(call_method(dev, 37, {0, rt}) == 0);
    CHECK(call_method(dev, 38, {0, sc(0x24)}) == 0 && com_this(rd32(sc(0x24))) == com_this(rt));
    call_method(rd32(sc(0x24)), 2);
    wr32(sc(0x24), 0x1234);
    CHECK(call_method(dev, 38, {1, sc(0x24)}) == not_found && rd32(sc(0x24)) == 0);
    // The vertex declaration: the one set, null after clearing it.
    const uint8_t decl[] = {0, 0, 0, 0, 3, 0, 0, 0, 0xff, 0, 0, 0, 17, 0, 0, 0};
    memcpy(gm_ptr(sc(0x300)), decl, sizeof decl);
    CHECK(call_method(dev, 86, {sc(0x300), sc(0x28)}) == 0);
    const uint32_t d = rd32(sc(0x28));
    CHECK(call_method(dev, 87, {d}) == 0);
    CHECK(call_method(dev, 88, {sc(0x2c)}) == 0 && com_this(rd32(sc(0x2c))) == com_this(d));
    call_method(rd32(sc(0x2c)), 2);
    CHECK(call_method(dev, 87, {0}) == 0);
    CHECK(call_method(dev, 88, {sc(0x2c)}) == 0 && rd32(sc(0x2c)) == 0);
    call_method(d, 2);
    // GetRenderTargetData queues the copy; a DONOTWAIT lock answers WASSTILLDRAWING until the
    // renderer has it, then locks the delivered image. A lock without the flag waits.
    CHECK(call_method(dev, 36, {4, 4, 21, 2, sc(0x30), 0}) == 0);
    const uint32_t sys = rd32(sc(0x30));
    CHECK(call_method(dev, 32, {rt, sys}) == 0);
    CHECK(rb_requests == 1 && rb_ticket == com_this(sys)->id && rb_source == com_this(rt)->id);
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4000}) == still_drawing);
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4010}) == still_drawing);
    rb_ready = 1;
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4010}) == 0);
    const uint32_t pitch = rd32(sc(0x40)), bits = rd32(sc(0x44));
    CHECK(pitch >= 16 && rd32(bits) == 0x11000000u && rd32(bits + pitch + 4) == 0x11000011u);
    CHECK(call_method(sys, 14) == 0);
    const int forgets = rb_forgets;
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4000}) == 0); // nothing pending: plain lock
    CHECK(call_method(sys, 14) == 0 && rb_forgets == forgets);
    CHECK(call_method(dev, 32, {rt, sys}) == 0 && rb_requests == 2);
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0}) == 0 && rb_sync_reads == 1);
    CHECK(rd32(rd32(sc(0x44))) == 0x55555555u);
    CHECK(call_method(sys, 14) == 0);
    // Sizes must match.
    CHECK(call_method(dev, 36, {8, 4, 21, 2, sc(0x34), 0}) == 0);
    const uint32_t wide = rd32(sc(0x34));
    CHECK(call_method(dev, 32, {rt, wide}) == invalid && rb_requests == 2);
    call_method(wide, 2);
    CHECK(call_method(dev, 32, {rt, sys}) == 0 && rb_requests == 3);
    call_method(sys, 2); // destroyed while pending: the ticket is forgotten
    CHECK(rb_forgets > forgets);
    CHECK(call_method(dev, 37, {0, original_target}) == 0);
    call_method(original_target, 2);
    call_method(rt, 2);
}


// Test155: the four draw calls refuse a primitive count above the MaxPrimitiveCount this device reports in its caps
// (and indexed draws a vertex count above MaxVertexIndex + 1) with D3DERR_INVALIDCALL, before any size arithmetic,
// for every topology, and without growing the process. At the limit they are accepted.
static void test_draw_limits(uint32_t dev) {
    const uint32_t invalid = 0x8876086cu;
    CHECK(call_method(dev, 7, {sc(0x800)}) == 0); // GetDeviceCaps
    const uint32_t max_prims = rd32(sc(0x800) + 180), max_index = rd32(sc(0x800) + 184);
    CHECK(max_prims == 1048575 && max_index == 1048575);
    memset(gm_ptr(sc(0x1000)), 0, 0x1000); // degenerate vertices and indices
    struct rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    for (uint32_t prim = 1; prim <= 6; ++prim) {
        for (uint32_t count : {max_prims + 1, 0x7fffffffu, 0x80000000u, 0x80000000u + 350u, 0xffffffffu}) {
            CHECK(call_method(dev, 83, {prim, count, sc(0x1000), 16}) == invalid);          // DrawPrimitiveUP
            CHECK(call_method(dev, 84, {prim, 0, 3, count, sc(0x1000), 101, sc(0x1000), 16}) == invalid);
            CHECK(call_method(dev, 81, {prim, 0, count}) == invalid);                        // DrawPrimitive
            CHECK(call_method(dev, 82, {prim, 0, 0, 3, 0, count}) == invalid);              // DrawIndexedPrimitive
        }
        // vertex count over the index limit, primitive count fine
        CHECK(call_method(dev, 84, {prim, 0, max_index + 2, 1, sc(0x1000), 101, sc(0x1000), 16}) == invalid);
        CHECK(call_method(dev, 82, {prim, 0, 0, max_index + 2, 0, 1}) == invalid);
        // small valid draws are accepted
        CHECK(call_method(dev, 83, {prim, 1, sc(0x1000), 16}) == 0);
        CHECK(call_method(dev, 84, {prim, 0, 3, 1, sc(0x1000), 101, sc(0x1000), 16}) == 0);
    }
    // at and just below the limit: accepted (point lists, so the vertex count equals the primitive count; the
    // degenerate data lives anywhere in the arena)
    for (uint32_t count : {max_prims - 1, max_prims}) {
        CHECK(call_method(dev, 81, {1, 0, count}) == 0);
        CHECK(call_method(dev, 83, {1, count, 0x00100000u, 16}) == 0);
    }
    getrusage(RUSAGE_SELF, &after);
    // ru_maxrss: bytes on macOS, kilobytes on Linux; either way far below a runaway allocation
    CHECK((uint64_t)(after.ru_maxrss - before.ru_maxrss) < (uint64_t)512 * 1024 * 1024);
}

// Test155: with no depth-stencil surface set, GetDepthStencilSurface returns D3DERR_NOTFOUND and a null pointer
// (Direct3D 9), every time, and makes nothing; with one set it returns that one.
static void test_depth_stencil_get(uint32_t dev) {
    const uint32_t notfound = 0x88760866u, out = sc(0x900), made = sc(0x904);
    CHECK(call_method(dev, 39, {0}) == 0); // SetDepthStencilSurface(NULL)
    for (int i = 0; i < 64; ++i) {
        wr32(out, 0xdeadbeefu);
        CHECK(call_method(dev, 40, {out}) == notfound);
        CHECK(rd32(out) == 0);
    }
    CHECK(call_method(dev, 29, {16, 16, 75, 0, 0, 0, made, 0}) == 0); // CreateDepthStencilSurface D24S8
    const uint32_t surface = rd32(made);
    CHECK(surface != 0);
    CHECK(call_method(dev, 39, {surface}) == 0);
    CHECK(call_method(dev, 40, {out}) == 0);
    CHECK(rd32(out) != 0);
    if (rd32(out))
        call_method(rd32(out), 2); // Release the reference GetDepthStencilSurface added
    CHECK(call_method(dev, 39, {0}) == 0);
    CHECK(call_method(dev, 40, {out}) == notfound);
    if (surface)
        call_method(surface, 2);
}

// Test157c: depth-stencil reference rules of Direct3D 9 (the audit found the kit diverged in all three):
// SetDepthStencilSurface AddRefs the bound surface (the device keeps it alive after the app releases it), and
// Reset drops only the device's own references, never the app's.
static void test_depth_stencil_refs(uint32_t dev) {
    const uint32_t made = sc(0x910), out = sc(0x914), pp = sc(0x940);
    auto addref = [](uint32_t o) { return call_method(o, 1); };
    auto release = [](uint32_t o) { return call_method(o, 2); };
    CHECK(call_method(dev, 29, {16, 16, 75, 0, 0, 0, made, 0}) == 0);
    const uint32_t s = rd32(made), s_id = com_this(s)->id;
    CHECK(call_method(dev, 39, {s}) == 0);
    CHECK(addref(s) == 3); // app + device + this
    release(s);
    CHECK(release(s) == 1); // the app lets go; the device still holds it
    CHECK(com_get(s_id) != nullptr);
    wr32(out, 0);
    CHECK(call_method(dev, 40, {out}) == 0);
    CHECK(rd32(out) && com_this(rd32(out)) && com_this(rd32(out))->id == s_id);
    if (rd32(out))
        release(rd32(out));
    CHECK(call_method(dev, 39, {0}) == 0); // unbinding drops the last reference
    CHECK(com_get(s_id) == nullptr);
    // Reset with the automatic depth buffer on, while an app-owned surface is bound
    CHECK(call_method(dev, 29, {16, 16, 75, 0, 0, 0, made, 0}) == 0);
    const uint32_t e = rd32(made), e_id = com_this(e)->id;
    CHECK(call_method(dev, 39, {e}) == 0);
    memset(gm_ptr(pp), 0, 56);
    wr32(pp, 8);
    wr32(pp + 4, 8);
    wr32(pp + 36, 1);
    wr32(pp + 40, 75);
    CHECK(call_method(dev, 16, {pp}) == 0);
    CHECK(com_get(e_id) != nullptr); // the app's surface survives Reset
    CHECK(addref(e) == 2);           // app + this: the device let go of its binding only
    release(e);
    wr32(out, 0);
    CHECK(call_method(dev, 40, {out}) == 0); // the new automatic depth buffer is bound
    const uint32_t auto1 = rd32(out) && com_this(rd32(out)) ? com_this(rd32(out))->id : 0;
    CHECK(auto1 != 0 && auto1 != e_id);
    if (rd32(out))
        release(rd32(out));
    CHECK(release(e) == 0); // the app's last reference
    CHECK(com_get(e_id) == nullptr);
    // a second Reset replaces the automatic depth buffer; the old one goes once nothing holds it
    CHECK(call_method(dev, 16, {pp}) == 0);
    CHECK(com_get(auto1) == nullptr);
    wr32(out, 0);
    CHECK(call_method(dev, 40, {out}) == 0 && rd32(out));
    if (rd32(out))
        release(rd32(out));
}

// Test159: a texture level shares its texture's lifetime (Direct3D 9): releasing the level gives back the texture
// reference it took, in either release order, and a level obtained twice needs two releases.
static void test_surface_container_refs(uint32_t dev) {
    const uint32_t texp = sc(0x980), surfp = sc(0x984), surf2p = sc(0x988);
    auto release = [](uint32_t o) { return call_method(o, 2); };
    for (int order = 0; order < 2; ++order) {
        CHECK(call_method(dev, 23, {16, 16, 1, 1 /* RENDERTARGET */, 21, 0, texp, 0}) == 0); // CreateTexture
        const uint32_t tex = rd32(texp), tex_id = com_this(tex)->id;
        CHECK(call_method(tex, 18, {0, surfp}) == 0); // GetSurfaceLevel
        const uint32_t surf = rd32(surfp), surf_id = com_this(surf)->id;
        if (order == 0) {
            release(tex);                              // the app lets the texture go first...
            CHECK(com_get(tex_id) != nullptr);         // ...the level keeps it alive
            release(surf);
        } else {
            release(surf);
            CHECK(com_get(tex_id) != nullptr);         // the app still holds the texture
            release(tex);
        }
        CHECK(com_get(tex_id) == nullptr && com_get(surf_id) == nullptr);
    }
    // a level obtained twice, and through GetRenderTarget after SetRenderTarget
    CHECK(call_method(dev, 23, {16, 16, 1, 1, 21, 0, texp, 0}) == 0);
    const uint32_t tex = rd32(texp), tex_id = com_this(tex)->id;
    CHECK(call_method(tex, 18, {0, surfp}) == 0);
    CHECK(call_method(tex, 18, {0, surf2p}) == 0);
    const uint32_t s1 = rd32(surfp), s2 = rd32(surf2p);
    CHECK(call_method(dev, 37, {0, s1}) == 0);    // SetRenderTarget(0, level)
    CHECK(call_method(dev, 38, {0, surfp}) == 0); // GetRenderTarget(0)
    const uint32_t s3 = rd32(surfp);
    CHECK(call_method(dev, 38, {1, surfp}) != 0 || rd32(surfp) == 0); // nothing on slot 1
    CHECK(call_method(dev, 18, {0, 0, 0, surf2p + 4}) == 0); // GetBackBuffer: put the back buffer back on slot 0
    const uint32_t bb = rd32(surf2p + 4);
    CHECK(call_method(dev, 37, {0, bb}) == 0);
    release(bb);
    release(tex);
    release(s1);
    release(s2);
    CHECK(com_get(tex_id) != nullptr); // GetRenderTarget's reference is still outstanding
    release(s3);
    CHECK(com_get(tex_id) == nullptr);
}

// D3DXLoadSurfaceFromSurface into DXT1 keeps 1-bit alpha as D3DX does: a pixel under half alpha is the block's
// transparent colour. NFSC draws a car's vinyls into an A8R8G8B8 skin, transparent where there is no vinyl, and
// copies it into DXT1 (007d7580 -> 00732ca0); opaque blocks turned the paint under a vinyl black.
// Blocks: all transparent, all opaque red, and transparent on the left half / opaque blue on the right half.
static void test_dxt1_alpha_copy(uint32_t dev) {
    const uint32_t texp = sc(0x9a0), dxtp = sc(0x9a4), ssp = sc(0x9a8), dsp = sc(0x9ac), lr = sc(0x9b0);
    CHECK(call_method(dev, 23, {12, 4, 1, 0, 21 /* A8R8G8B8 */, 1 /* MANAGED */, texp, 0}) == 0); // CreateTexture
    CHECK(call_method(dev, 23, {12, 4, 1, 0, 0x31545844u /* DXT1 */, 1, dxtp, 0}) == 0);
    const uint32_t tex = rd32(texp), dxt = rd32(dxtp);
    auto want_clear = [](uint32_t x) { return x < 4 || (x >= 8 && x < 10); };
    CHECK(call_method(tex, 19, {0, lr, 0, 0}) == 0); // LockRect
    const uint32_t pitch = rd32(lr), bits = rd32(lr + 4);
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 12; ++x)
            wr32(bits + y * pitch + x * 4, want_clear(x) ? 0u : x < 8 ? 0xffff0000u : 0xff0000ffu);
    CHECK(call_method(tex, 20, {0}) == 0); // UnlockRect
    CHECK(call_method(tex, 18, {0, ssp}) == 0); // GetSurfaceLevel
    CHECK(call_method(dxt, 18, {0, dsp}) == 0);
    const uint32_t ss = rd32(ssp), ds = rd32(dsp);
    CHECK(call_shim(tramp("d3dx9_26.dll", "D3DXLoadSurfaceFromSurface"),
                    {ds, 0, 0, ss, 0, 0, 1 /* D3DX_FILTER_NONE */, 0}) == 0);
    CHECK(call_method(dxt, 19, {0, lr, 0, 0x10 /* READONLY */}) == 0);
    const uint32_t blocks = rd32(lr + 4);
    for (uint32_t bx = 0; bx < 3; ++bx) {
        const uint32_t ends = rd32(blocks + bx * 8), idx = rd32(blocks + bx * 8 + 4);
        const uint32_t c0 = ends & 0xffff, c1 = ends >> 16;
        for (uint32_t k = 0; k < 16; ++k) {
            const uint32_t i = idx >> (2 * k) & 3, x = bx * 4 + k % 4;
            const bool clear = c0 <= c1 && i == 3; // 3-colour mode's transparent index
            CHECK(clear == want_clear(x));
            if (!clear && i < 2)
                CHECK((i == 0 ? c0 : c1) == (x < 8 ? 0xf800u : 0x001fu)); // red / blue as R5G6B5
        }
    }
    CHECK(call_method(dxt, 20, {0}) == 0);
    for (uint32_t o : {ss, ds, tex, dxt})
        call_method(o, 2);
}

static uint32_t make_device() {
    const uint32_t api = call_shim(tramp("d3d9.dll", "Direct3DCreate9"), {32});
    CHECK(api != 0);
    wr32(sc(0x100), 8);
    wr32(sc(0x104), 8);
    CHECK(call_method(api, 16, {0, 1, 0, 0, sc(0x100), sc(0)}) == 0);
    const uint32_t dev = rd32(sc(0));
    CHECK(dev != 0);
    call_method(api, 2);
    return dev;
}

static void test_device_booleans(uint32_t dev) {
    // D3D9 vtable Set/Get VS B=98/99, PS B=113/114. The actual guest
    // stack and vtable calls catch ABI errors as well as bounds mistakes.
    for (uint32_t set : {98u, 113u}) {
        for (uint32_t i = 0; i < 16; ++i)
            wr32(sc(0x200) + i * 4, i & 1 ? 0xfffffff9u : 0);
        CHECK(call_method(dev, set, {0, sc(0x200), 16}) == 0);
        CHECK(call_method(dev, set + 1, {0, sc(0x300), 16}) == 0);
        CHECK(memcmp(gm_ptr(sc(0x200)), gm_ptr(sc(0x300)), 64) == 0);
        wr32(sc(0x200), 3);
        for (const auto range :
             {std::array<uint32_t, 2>{15, 2}, {16, 1}, {0xffffffffu, 1}, {0, 0xffffffffu}}) {
            CHECK(call_method(dev, set, {range[0], sc(0x200), range[1]}) == 0x8876086cu);
            wr32(sc(0x300), 0xdeadbeefu);
            CHECK(call_method(dev, set + 1, {range[0], sc(0x300), range[1]}) == 0x8876086cu);
            CHECK(rd32(sc(0x300)) == 0xdeadbeefu);
        }
        CHECK(call_method(dev, set + 1, {15, sc(0x300), 1}) == 0);
        CHECK(rd32(sc(0x300)) == 0xfffffff9u); // invalid writes were atomic
        CHECK(call_method(dev, set, {0, 0, 1}) == 0x8876086cu);
        CHECK(call_method(dev, set + 1, {0, 0, 1}) == 0x8876086cu);
        CHECK(call_method(dev, set, {0, 0xfffffffcu, 2}) == 0x8876086cu);
        CHECK(call_method(dev, set + 1, {0, 0xfffffffcu, 2}) == 0x8876086cu);
    }
}

static void test_effect_booleans(uint32_t dev) {
    const auto fx = effect();
    // Minimal loaded PE resource directory. This exercises the same export
    // used by a game, with an allocated guest module instead of a disk image.
    const uint32_t module = heap_alloc(0x1000 + uint32_t(fx.size()), true, 16);
    CHECK(module != 0);
    wr32(module, 0x5a4d);
    wr32(module + 0x3c, 0x40);
    wr32(module + 0x40, 0x4550);
    wr32(module + 0x40 + 0x18 + 0x60 + 16, 0x200);
    const uint32_t root = module + 0x200;
    wr32(root + 12, 1u << 16);
    wr32(root + 16, 10);
    wr32(root + 20, 0x80000020u);
    wr32(root + 0x20 + 12, 1u << 16);
    wr32(root + 0x20 + 16, 1);
    wr32(root + 0x20 + 20, 0x80000040u);
    wr32(root + 0x40 + 12, 1u << 16);
    wr32(root + 0x40 + 16, 1033);
    wr32(root + 0x40 + 20, 0x60);
    wr32(root + 0x60, 0x1000);
    wr32(root + 0x64, uint32_t(fx.size()));
    memcpy(gm_ptr(module + 0x1000), fx.data(), fx.size());
    CHECK(call_shim(tramp("d3dx9_26.dll", "D3DXCreateEffectFromResourceA"),
                    {dev, module, 1, 0, 0, 0, 0, sc(0), 0}) == 0);
    const uint32_t e = rd32(sc(0));
    CHECK(e != 0);
    memcpy(gm_ptr(sc(0x200)), "Switch", 7);
    memcpy(gm_ptr(sc(0x220)), "Many", 5);
    const uint32_t s = call_method(e, 9, {0, sc(0x200)}), m = call_method(e, 9, {0, sc(0x220)});
    CHECK(s != 0 && m != 0 && s != m);
    CHECK(call_method(e, 63, {sc(0x20), 0}) == 0);
    CHECK(rd32(sc(0x20)) == 1);
    CHECK(call_method(e, 64, {0}) == 0);
    D9Pipeline &pl = d9_pipeline(com_this(dev)->id);
    CHECK(pl.vbool[15] == 0 && pl.pbool[0] == 0 && pl.pbool[1] == 1);
    CHECK(!pl.vs.empty() && !pl.ps.empty());
    for (uint32_t value : {1u, 0u, 1u}) {
        CHECK(call_method(e, 22, {s, value}) == 0);
        wr32(sc(0x240), value);
        wr32(sc(0x244), !value);
        CHECK(call_method(e, 24, {m, sc(0x240), 2}) == 0);
        CHECK(call_method(e, 65) == 0);
        CHECK(pl.vbool[15] == int32_t(value));
        CHECK(pl.pbool[0] == int32_t(value) && pl.pbool[1] == int32_t(!value));
    }
    // Test75: incremental CommitChanges (d3dx9.cpp bind_constants). The registers remember which
    // effect and shader wrote them; an unchanged commit keeps the values, a parameter written between
    // two commits reaches the registers, and a device constant setter in between makes the next
    // commit write every constant again (the shim's behaviour before Test75 too).
    CHECK(pl.vbinder.fx == com_this(e)->id && pl.pbinder.fx == com_this(e)->id);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 1 && pl.pbool[0] == 1 && pl.pbool[1] == 0);
    wr32(sc(0x260), 0);
    CHECK(call_method(dev, 98, {15, sc(0x260), 1}) == 0); // SetVertexShaderConstantB
    CHECK(pl.vbool[15] == 0 && pl.vbinder.fx == 0);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 1 && pl.vbinder.fx == com_this(e)->id);
    CHECK(call_method(e, 22, {s, 0}) == 0);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 0 && pl.pbool[0] == 1);
    CHECK(call_method(e, 22, {s, 1}) == 0);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 1);
    const uint8_t decl[] = {0, 0, 0, 0, 2, 0, 0, 0, 0xff, 0, 0, 0, 17, 0, 0, 0};
    memcpy(gm_ptr(sc(0x300)), decl, sizeof decl);
    CHECK(call_method(dev, 86, {sc(0x300), sc(0)}) == 0);
    const uint32_t declaration = rd32(sc(0));
    CHECK(call_method(dev, 87, {declaration}) == 0);
    const float triangle[] = {-1, 1, 0.5f, 3, 1, 0.5f, -1, -3, 0.5f};
    memcpy(gm_ptr(sc(0x400)), triangle, sizeof triangle);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(draws == 1);
    CHECK(captured.vbool[15] == 1 && captured.pbool[0] == 1 && captured.pbool[1] == 0);
    CHECK(captured.admission.serial == 0 && rejected_draws == 0);
    // Finish the legacy first-eight draw descriptions using harmless no-IB
    // calls before supplying deliberately invalid UP pointers below.
    for (int i = 0; i < 3; ++i)
        CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(rejected_draws == 0 && draws == 1);
    admission_enabled = true;
    CHECK(call_method(dev, 26, {36, 0, 0, 0, sc(0x50), 0}) == 0);
    uint32_t vb = rd32(sc(0x50));
    CHECK(call_method(dev, 100, {0, vb, 0, 12}) == 0);
    CHECK(call_method(dev, 27, {6, 0, 101, 0, sc(0x54), 0}) == 0);
    uint32_t ib = rd32(sc(0x54));
    CHECK(call_method(dev, 104, {ib}) == 0);
    CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(draws == 2 && captured.admission.serial != 0);
    CHECK(captured.admission.api == 82 && captured.admission.device == dev);
    CHECK(captured.admission.caller != 0);
    CHECK(captured.admission.resource_identity[0] == vb);
    CHECK(captured.admission.resource_identity[8] == ib);
    uint64_t serial = captured.admission.serial;
    CHECK(call_method(dev, 104, {0}) == 0);
    CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(rejected_draws == 1 && draws == 2 && rejected.admission.reason == 3);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 82);
    CHECK(rejected.stream[0].buffer == com_this(vb)->id);
    CHECK(rejected.admission.resource_identity[0] == vb);
    CHECK(rejected.target.color[0].id == captured.target.color[0].id);
    CHECK(call_method(dev, 87, {0}) == 0);
    CHECK(call_method(dev, 81, {4, 0, 1}) == 0);
    CHECK(rejected_draws == 2 && draws == 2 && rejected.admission.reason == 2);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 81);
    CHECK(call_method(dev, 87, {declaration}) == 0);
    auto saved_vs = std::move(pl.vs);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(rejected_draws == 3 && draws == 2 && rejected.admission.reason == 1);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 83);
    pl.vs = std::move(saved_vs);
    CHECK(call_method(dev, 83, {4, 1, 0xfffffff0u, 12}) == 0);
    CHECK(rejected_draws == 4 && rejected.admission.reason == 4);
    CHECK(rejected.admission.serial == ++serial);
    CHECK(call_method(dev, 84, {4, 0, 3, 1, 0xfffffff0u, 101, sc(0x400), 12}) == 0);
    CHECK(rejected_draws == 5 && draws == 2 && rejected.admission.reason == 4);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 84);
    CHECK(!rejected.inline_vertices && !rejected.inline_indices);
    CHECK(rejected.admission.inline_indices == 0xfffffff0u);
    CHECK(rejected.admission.inline_vertices == sc(0x400));
    CHECK(rejected.admission.resource_identity[0] == 0);
    CHECK(rejected.stream[0].buffer == 0 && rejected.index_buffer == 0);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(draws == 3 && captured.admission.reason == 0);
    CHECK(captured.admission.serial == ++serial && captured.admission.api == 83);
    admission_enabled = false;
    CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(rejected_draws == 5 && draws == 3); // opt-out retains old behavior
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(draws == 4 && captured.admission.serial == 0);
    call_method(dev, 100, {0, 0, 0, 0});
    call_method(vb, 2);
    call_method(ib, 2);
    call_method(e, 66);
    call_method(e, 67);
    call_method(e, 2);
    call_method(declaration, 2);
}

int main() {
    test_volume_dds();
    mem_init();
    imports_init();
    dx_register_shims();
    g_stack_top = STACK_TOP - 0x1000;
    g_scratch = heap_alloc(0x4000, true, 16);
    if (!g_scratch)
        return 1;
    cpu_reset();
    const uint32_t dev = make_device();
    if (!dev)
        return 1;
    test_device_booleans(dev);
    test_draw_limits(dev);
    test_depth_stencil_get(dev);
    test_surface_container_refs(dev);
    test_dxt1_alpha_copy(dev);
    test_effect_booleans(dev);
    test_volume(dev);
    test_state_getters_and_readback(dev);
    const uint32_t bound = make_volume(dev);
    const uint32_t bound_id = com_this(bound)->id;
    const uint32_t child_id = com_this(bound)->surfaces[0];
    CHECK(call_method(dev, 65, {0, bound}) == 0);
    CHECK(call_method(bound, 2) == 1);
    call_method(dev, 2);
    CHECK(!com_get(bound_id) && !com_get(child_id));
    if (const uint32_t refs_dev = make_device()) // Test157c, on a device of its own (it Resets)
        test_depth_stencil_refs(refs_dev);
    printf("d3d9 guest/effect: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
