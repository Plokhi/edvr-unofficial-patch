// The map temporal-off frames' upscale (2026-10-10, map_easu_shader.h): the spatial recovery with the frame's easu flag runs the real precompiled
// passes (mapCompress, then mapEasu) on the rig's WARP device, and its output is checked against the same recovery's bilinear tap. The
// input is the copy route's 8-bit colour (R8G8B8A8_UNORM, what inputTexture accepts there): a 960x540 gradient with bright dots, recovered
// to 1920x1080 (DLSS below native: the evaluation size is the display size). The flight of 6899f14b showed the EASU output black on the
// maps with the UI layer composited over it; this rig is the gate for that.
//
// Checks: the EASU path engages and recovers; the output is the display size; no NaN or Inf; the output is not black (its mean is at least
// half the bilinear reference's); and the mean absolute error to the bilinear reference is under a bound (8-bit full scale = 1.0).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mapeasugpu {

inline float halfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) bits = sign;
        else {
            // subnormal: normalise
            uint32_t m = man, e = 127 - 15 + 1;
            while (!(m & 0x400u)) { m <<= 1; --e; }
            m &= 0x3FFu;
            bits = sign | (e << 23) | (m << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Reads a 2D texture behind a view back to RGBA floats in [0, 1] for 8-bit UNORM, or as half floats. Returns false for another format.
inline bool readRgba(ID3D11Device* d, ID3D11DeviceContext* c, ID3D11ShaderResourceView* srv, std::vector<float>& out, UINT& w, UINT& h,
                     DXGI_FORMAT& fmt) {
    ComPtr<ID3D11Resource> res;
    srv->GetResource(res.GetAddressOf());
    ComPtr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    // The output is typeless (R8G8B8A8_TYPELESS, the resolver's non-TAA output); the view says how its bytes are read.
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    srv->GetDesc(&viewDesc);
    const DXGI_FORMAT viewFormat = viewDesc.Format;
    w = desc.Width;
    h = desc.Height;
    fmt = viewFormat;
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(d->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()))) return false;
    c->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    bool ok = true;
    out.assign(size_t(w) * h * 4, 0.0f);
    for (UINT y = 0; y < h && ok; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            for (int ch = 0; ch < 4; ++ch) {
                float v = 0.0f;
                if (viewFormat == DXGI_FORMAT_R8G8B8A8_UNORM || viewFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                    v = row[x * 4 + ch] / 255.0f;   // the stored bytes, as the resolver wrote them
                else if (viewFormat == DXGI_FORMAT_R16G16B16A16_FLOAT)
                    v = halfToFloat(reinterpret_cast<const uint16_t*>(row)[x * 4 + ch]);
                else { ok = false; break; }
                out[(size_t(y) * w + x) * 4 + ch] = v;
            }
            if (!ok) break;
        }
    }
    c->Unmap(staging.Get(), 0);
    return ok;
}

}  // namespace mapeasugpu

// The maintainer's test content (axis-aligned lines, a circle, a tilted ring, the slope edges) through EASU alone and SMAA ahead of EASU
// (2026-10-10), with the jaggedness gates (flat_map_shapes_gpu_tests.h).
#include "flat_map_shapes_gpu_tests.h"

// SMAA is below native only (2026-10-10): a native frame runs EASU alone, with the smaa flag on.
inline void mapSmaaNativeTest(ID3D11Device* device, ID3D11DeviceContext* context) {
    const UINT nW = 1920, nH = 1080;
    std::vector<uint8_t> np(size_t(nW) * nH * 4);
    for (UINT y = 0; y < nH; ++y)
        for (UINT x = 0; x < nW; ++x) {
            uint8_t* p = &np[(size_t(y) * nW + x) * 4];
            p[0] = uint8_t(x * 255u / (nW - 1));
            p[1] = uint8_t(y * 255u / (nH - 1));
            p[2] = 128;
            p[3] = 255;
        }
    auto nativeIn = texture(device, nW, nH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, np.data(), nW * 4);
    if (!nativeIn) { check(false, "map SMAA native: the input texture is made"); return; }
    auto nativeView = view(device, nativeIn.Get());
    edvr::FlatMonoResolveFrame n{};
    n.color = nativeView.Get();
    n.renderWidth = nW;
    n.renderHeight = nH;
    n.outputWidth = nW;
    n.outputHeight = nH;
    n.mode = edvr::FlatMonoResolveMode::Dlss;
    n.hdr = false;
    n.reset = true;
    n.frame = 1;
    n.easu = true;
    n.smaa = true;
    ComPtr<ID3D11ShaderResourceView> nativeOut;
    const char* why = nullptr;
    const bool ok = edvr::flatMonoResolveSpatialFallback(device, context, n, nativeOut.GetAddressOf(), &why);
    check(ok && nativeOut && edvr::flatMonoResolveLastSpatialWasEasu() && !edvr::flatMonoResolveLastSpatialWasSmaa(),
          "map SMAA native: a native frame runs EASU alone (SMAA is below native only)");
    if (!ok) std::printf("  (native reason: %s)\n", why ? why : "none");
}

// The rig's own calls: check() and failures are the main file's. The input is built here, then the bilinear reference and the EASU
// recovery run through the real spatial recovery of this file's resolver.
inline void mapEasuGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    const UINT inW = 960, inH = 540, outW = 1920, outH = 1080;
    std::vector<uint8_t> px(size_t(inW) * inH * 4);
    for (UINT y = 0; y < inH; ++y)
        for (UINT x = 0; x < inW; ++x) {
            uint8_t* p = &px[(size_t(y) * inW + x) * 4];
            p[0] = uint8_t(x * 255u / (inW - 1));
            p[1] = uint8_t(y * 255u / (inH - 1));
            p[2] = uint8_t(96u + ((x + y) & 31u));
            p[3] = 255;
            if (x % 37u == 18u && y % 29u == 14u) { p[0] = p[1] = p[2] = 255; }   // bright dots: EASU's edge case
        }
    auto input = texture(device, inW, inH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, px.data(), inW * 4);
    if (!input) { check(false, "map EASU rig: the 960x540 input texture is made"); return; }
    auto inputView = view(device, input.Get());

    edvr::FlatMonoResolveFrame f{};
    f.color = inputView.Get();
    f.renderWidth = inW;
    f.renderHeight = inH;
    f.outputWidth = outW;
    f.outputHeight = outH;
    f.mode = edvr::FlatMonoResolveMode::Dlss;
    f.hdr = false;
    f.reset = true;
    f.frame = 1;

    // The bilinear reference: the same recovery without the flag.
    f.easu = false;
    ComPtr<ID3D11ShaderResourceView> bilinear;
    const char* why = nullptr;
    const bool okBilinear = edvr::flatMonoResolveSpatialFallback(device, context, f, bilinear.GetAddressOf(), &why);
    check(okBilinear && bilinear && !edvr::flatMonoResolveLastSpatialWasEasu(),
          "map EASU rig: the bilinear reference recovers the frame, and it does not run EASU");
    if (!okBilinear || !bilinear) {
        std::printf("  (bilinear reason: %s)\n", why ? why : "none");
        return;
    }
    // Both recoveries write the same output texture (g.output[1]), so the reference is read back NOW, before the EASU call overwrites it.
    std::vector<float> b;
    UINT bw = 0, bh = 0;
    DXGI_FORMAT bfmt{};
    const bool readB = mapeasugpu::readRgba(device, context, bilinear.Get(), b, bw, bh, bfmt);
    check(readB, "map EASU rig: the bilinear reference reads back (8-bit UNORM)");
    if (!readB) return;

    // The map frames' recovery: the easu flag, as flat_runtime's map temporal-off branch sets it.
    f.easu = true;
    ComPtr<ID3D11ShaderResourceView> easu;
    why = nullptr;
    const bool okEasu = edvr::flatMonoResolveSpatialFallback(device, context, f, easu.GetAddressOf(), &why);
    const bool engaged = edvr::flatMonoResolveLastSpatialWasEasu();
    check(okEasu && easu && engaged, "map EASU rig: the EASU path recovers the frame, and the easu flag engages it");
    if (!okEasu || !easu) {
        std::printf("  (EASU reason: %s)\n", why ? why : "none");
        return;
    }

    std::vector<float> a;
    UINT aw = 0, ah = 0;
    DXGI_FORMAT afmt{};
    const bool readA = mapeasugpu::readRgba(device, context, easu.Get(), a, aw, ah, afmt);
    check(readA, "map EASU rig: the EASU output reads back (8-bit UNORM)");
    if (!readA) return;
    check(aw == outW && ah == outH && bw == outW && bh == outH,
          "map EASU rig: both outputs are the display size (1920x1080)");
    if (aw != outW || ah != outH || bw != outW || bh != outH) return;

    double sumA = 0.0, sumB = 0.0, mae = 0.0, maxErr = 0.0;
    size_t nonFinite = 0;
    const size_t count = a.size();
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(a[i])) ++nonFinite;
        if (!std::isfinite(b[i])) ++nonFinite;
        if (i % 4 == 3) continue;   // alpha is 1 on both
        sumA += a[i];
        sumB += b[i];
        const double e = std::fabs(double(a[i]) - double(b[i]));
        mae += e;
        if (e > maxErr) maxErr = e;
    }
    const double pixels = double(outW) * outH;
    const double meanA = sumA / (pixels * 3.0), meanB = sumB / (pixels * 3.0);
    const double maeRgb = mae / (pixels * 3.0);
    std::printf("map EASU rig: EASU mean %.4f, bilinear mean %.4f, mean abs error %.4f, max %.4f, non-finite %zu, output %ux%u\n",
                meanA, meanB, maeRgb, maxErr, nonFinite, aw, ah);
    check(nonFinite == 0, "map EASU rig: the EASU output holds no NaN and no Inf");
    check(meanA > 0.05 && meanA > 0.5 * meanB,
          "map EASU rig: the EASU output is not black (its mean is at least half the bilinear reference's)");
    check(maeRgb < 0.04, "map EASU rig: the EASU output stays close to the bilinear reference (mean abs error under 0.04 of full scale)");
    mapSmaaNativeTest(device, context);
    mapShapesGpuTests(device, context);
}
