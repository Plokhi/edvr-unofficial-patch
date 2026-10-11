// The map's SMAA lookup textures (review of 0.19.1, finding 2): when AreaTex's or SearchTex's view fails after its texture was made, the
// texture must not leak, whether or not a retry follows. The failure is injected after the real texture is created (the seam in
// flat_mono_resolve.cpp, flatMonoResolveTestSmaaFault); each failed texture is handed to an observer that holds one COM reference. After the
// resolver is reset and the observer lets go, the count of references still standing must be 0 for every texture.
//
// Only a retry after a failure exposes the old leak: a reset alone frees the texture a failure left in the resolver's state, so each case
// runs a retry frame before its reset. Each frame is a below-native frame through the real spatial recovery with SMAA asked for.
//
// Negative control: the pre-fix ownership pattern (a second CreateTexture2D written into a ComPtr that still holds a live texture) goes
// through the same observer and must count one leaked reference. That proves the counting would catch the old behaviour.

#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

namespace edvr {
// Test-only seam and accessors, defined at the foot of flat_mono_resolve.cpp (not part of flat_mono_resolve.h's contract).
void flatMonoResolveTestSmaaFault(int areaFaults, int searchFaults, void (*observe)(ID3D11Texture2D*));
ID3D11Texture2D* flatMonoResolveTestSmaaTexture(bool area);
ID3D11ShaderResourceView* flatMonoResolveTestSmaaView(bool area);
}  // namespace edvr

namespace smaalookupgpu {

// Every texture an observer holds, one reference each, until releaseObserved().
inline std::vector<ID3D11Texture2D*>& observedTextures() {
    static std::vector<ID3D11Texture2D*> held;
    return held;
}
inline void observe(ID3D11Texture2D* texture) {
    if (!texture) return;
    texture->AddRef();
    observedTextures().push_back(texture);
}
// Holds the currently published AreaTex or SearchTex, if any, as an observer.
inline void observePublished(bool area) { observe(edvr::flatMonoResolveTestSmaaTexture(area)); }
// Lets go of every observer reference and returns the COM references still standing: 0 when nothing leaked.
inline unsigned releaseObserved() {
    unsigned left = 0;
    for (ID3D11Texture2D* texture : observedTextures()) left += texture->Release();
    observedTextures().clear();
    return left;
}

// A 960x540 gradient, as the map's 8-bit colour input.
inline ComPtr<ID3D11ShaderResourceView> lookupInput(ID3D11Device* device) {
    const UINT w = 960, h = 540;
    std::vector<uint8_t> pixels(size_t(w) * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            uint8_t* p = &pixels[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 255u / (w - 1));
            p[1] = uint8_t(y * 255u / (h - 1));
            p[2] = 128;
            p[3] = 255;
        }
    auto texture = ::texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, pixels.data(), w * 4);
    if (!texture) return ComPtr<ID3D11ShaderResourceView>();
    return view(device, texture.Get());
}

// One below-native frame (960x540 to 1920x1080) through the real spatial recovery, SMAA asked for. Returns true when the frame came back
// on EASU (the fallback is always EASU); smaaRan says whether the SMAA passes ran.
inline bool smaaFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* input, uint32_t frameNo, bool& smaaRan) {
    edvr::FlatMonoResolveFrame f{};
    f.color = input;
    f.renderWidth = 960;
    f.renderHeight = 540;
    f.outputWidth = 1920;
    f.outputHeight = 1080;
    f.mode = edvr::FlatMonoResolveMode::Dlss;
    f.hdr = false;
    f.reset = true;
    f.frame = frameNo;
    f.easu = true;
    f.smaa = true;
    ComPtr<ID3D11ShaderResourceView> out;
    const char* why = nullptr;
    const bool ok = edvr::flatMonoResolveSpatialFallback(device, context, f, out.GetAddressOf(), &why);
    smaaRan = edvr::flatMonoResolveLastSpatialWasSmaa();
    if (!ok || !out) std::printf("  (SMAA lookup frame %u reason: %s)\n", frameNo, why ? why : "none");
    return ok && out && edvr::flatMonoResolveLastSpatialWasEasu();
}

// The pre-fix ownership pattern: the held ComPtr is written again by CreateTexture2D without a release, as the old ensureSmaa did. The
// observer's count must show the overwritten texture as one standing reference.
inline unsigned legacyOverwriteControl(ID3D11Device* device) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 4;
    td.Height = 4;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> held;
    if (FAILED(device->CreateTexture2D(&td, nullptr, held.GetAddressOf()))) return 999;
    observe(held.Get());   // the failed attempt's texture, held by the observer as the rig holds it
    if (FAILED(device->CreateTexture2D(&td, nullptr, held.GetAddressOf()))) return 998;   // the retry overwrites the live pointer
    return releaseObserved();
}

}  // namespace smaalookupgpu

inline void mapSmaaLookupLeakTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace smaalookupgpu;
    ComPtr<ID3D11ShaderResourceView> in = lookupInput(device);
    if (!in) { check(false, "map SMAA lookups: the 960x540 input is made"); return; }
    uint32_t frameNo = 1;
    bool ran = false;

    // 1. The AreaTex view fails once; the retry publishes both lookups.
    {
        edvr::flatMonoResolveReset();
        edvr::flatMonoResolveTestSmaaFault(1, 0, observe);
        check(smaaFrame(device, context, in.Get(), frameNo++, ran) && !ran,
              "map SMAA lookups: an AreaTex view failure falls back to EASU");
        edvr::flatMonoResolveTestSmaaFault(0, 0, nullptr);
        check(!edvr::flatMonoResolveTestSmaaView(true) && !edvr::flatMonoResolveTestSmaaTexture(true),
              "map SMAA lookups: a failed AreaTex view publishes no AreaTex");
        check(observedTextures().size() == 1, "map SMAA lookups: the failed AreaTex texture reached the observer");
        check(smaaFrame(device, context, in.Get(), frameNo++, ran) && ran,
              "map SMAA lookups: the retry after an AreaTex failure runs SMAA");
        check(edvr::flatMonoResolveTestSmaaView(true) && edvr::flatMonoResolveTestSmaaView(false),
              "map SMAA lookups: the retry publishes both lookup views");
        observePublished(true);
        observePublished(false);
        edvr::flatMonoResolveReset();
        check(releaseObserved() == 0, "map SMAA lookups: AreaTex SRV failure then retry leaks no texture (refs 0 after reset)");
    }

    // 2. The SearchTex view fails once. AreaTex was complete and is kept (not re-created) by the retry.
    {
        edvr::flatMonoResolveReset();
        edvr::flatMonoResolveTestSmaaFault(0, 1, observe);
        check(smaaFrame(device, context, in.Get(), frameNo++, ran) && !ran,
              "map SMAA lookups: a SearchTex view failure falls back to EASU");
        edvr::flatMonoResolveTestSmaaFault(0, 0, nullptr);
        check(edvr::flatMonoResolveTestSmaaView(true) && !edvr::flatMonoResolveTestSmaaView(false),
              "map SMAA lookups: a failed SearchTex view publishes no SearchTex (AreaTex, complete, stays)");
        check(observedTextures().size() == 1, "map SMAA lookups: the failed SearchTex texture reached the observer");
        ID3D11Texture2D* area = edvr::flatMonoResolveTestSmaaTexture(true);
        check(smaaFrame(device, context, in.Get(), frameNo++, ran) && ran,
              "map SMAA lookups: the retry after a SearchTex failure runs SMAA");
        check(area && edvr::flatMonoResolveTestSmaaTexture(true) == area,
              "map SMAA lookups: the retry keeps the published AreaTex rather than re-creating it");
        observePublished(true);
        observePublished(false);
        edvr::flatMonoResolveReset();
        check(releaseObserved() == 0, "map SMAA lookups: SearchTex SRV failure then retry leaks no texture (refs 0 after reset)");
    }

    // 3. Repeated AreaTex failures: three frames in a row fail, the fourth succeeds.
    {
        edvr::flatMonoResolveReset();
        edvr::flatMonoResolveTestSmaaFault(3, 0, observe);
        bool allEasu = true;
        for (int i = 0; i < 3; ++i) allEasu = smaaFrame(device, context, in.Get(), frameNo++, ran) && !ran && allEasu;
        check(allEasu, "map SMAA lookups: three AreaTex failures in a row each fall back to EASU");
        check(observedTextures().size() == 3, "map SMAA lookups: each of the three failed AreaTex textures reached the observer");
        check(!edvr::flatMonoResolveTestSmaaView(true), "map SMAA lookups: no AreaTex view is published after repeated failures");
        edvr::flatMonoResolveTestSmaaFault(0, 0, nullptr);
        check(smaaFrame(device, context, in.Get(), frameNo++, ran) && ran,
              "map SMAA lookups: the frame after the injected failures runs SMAA");
        observePublished(true);
        observePublished(false);
        edvr::flatMonoResolveReset();
        check(releaseObserved() == 0, "map SMAA lookups: repeated AreaTex SRV failures leak no texture (refs 0 after reset)");
    }

    // 4. A clean pair published on a normal frame is released by reset (shutdown/reset).
    {
        edvr::flatMonoResolveReset();
        check(smaaFrame(device, context, in.Get(), frameNo++, ran) && ran, "map SMAA lookups: a clean below-native frame runs SMAA");
        observePublished(true);
        observePublished(false);
        edvr::flatMonoResolveReset();
        check(releaseObserved() == 0, "map SMAA lookups: reset releases the published lookup pair (refs 0)");
        check(!edvr::flatMonoResolveTestSmaaView(true) && !edvr::flatMonoResolveTestSmaaView(false),
              "map SMAA lookups: reset leaves no lookup view");
    }

    // 5. Negative control: the pre-fix overwrite pattern must count one leaked reference through the same observer.
    {
        edvr::flatMonoResolveReset();
        const unsigned control = legacyOverwriteControl(device);
        check(control == 1, "map SMAA lookups: negative control, the overwrite pattern is counted as one leaked reference");
        if (control != 1) std::printf("  (negative control count: %u)\n", control);
    }
    edvr::flatMonoResolveReset();
}
