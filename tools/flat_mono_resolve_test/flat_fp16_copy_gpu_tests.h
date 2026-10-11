// The copy route under an FP16 presentation (EDHDR's R16G16B16A16_FLOAT
// swapchain) on WARP: the structure admits the FP16 source (flat_copy_structure.h),
// the preflight accepts the FP16 view and allocates FP16 resources, and EDVR's
// own TAA -- which needs no SDK -- resolves a render-below-output frame and
// hands back an FP16 output with values above 1.0 intact (a UNORM trip would
// clamp them to 1.0). TAA only, so this runs on any Windows device; the SDK
// backends take the same HDR colour mode by construction (hdrColor).
#pragma once

inline void fp16CopyGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using edvr::FlatMonoResolveMode;
    // ---- the preflight knows the copy colour family --------------------------------------------------
    {
        edvr::FlatMonoResolvePreflight flight{};
        flight.renderWidth = 2246; flight.renderHeight = 1452;
        flight.outputWidth = 3456; flight.outputHeight = 2234;
        flight.mode = FlatMonoResolveMode::Taa; flight.hdr = false;
        flight.colorViewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        flight.depthViewFormat = DXGI_FORMAT_R32_FLOAT;
        const auto ready = edvr::flatMonoResolvePreflight(device, context, flight);
        check(ready.readyForRasterJitter(),
              "copy route: the 0.65x FP16 plan (2246x1452 into 3456x2234) preflights ready, TAA needs no SDK");
        auto hdrFp16 = flight; hdrFp16.hdr = true;
        const auto refused = edvr::flatMonoResolvePreflight(device, context, hdrFp16);
        check(refused.status == edvr::FlatMonoResolvePreflightStatus::InvalidMetadata && refused.reason &&
              !std::strcmp(refused.reason, "flat-preflight-hdr-requires-render-size-evaluation"),
              "HDR route: an FP16 R<D plan is refused by the render-size guard (its input is R11G11B10_FLOAT)");
        auto sdr = flight; sdr.colorViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        check(edvr::flatMonoResolvePreflight(device, context, sdr).readyForRasterJitter(),
              "copy route: the same plan in UNORM still preflights ready");
    }
    // ---- the resolve keeps FP16 end to end ------------------------------------------------------------
    // 2.5 in fp16 is 0x4100: above 1.0 on purpose, so a UNORM trip anywhere
    // in the path reads back 0x3C00 instead.
    constexpr UINT w = 16, h = 12, outW = 32, outH = 24;
    constexpr uint16_t kHalfTwoPointFive = 0x4100;
    edvr::flatMonoResolveReset();
    std::vector<uint8_t> colorBytes(size_t(w) * h * 8);
    for (size_t i = 0; i < colorBytes.size(); i += 2) {
        colorBytes[i] = uint8_t(kHalfTwoPointFive & 0xFF);
        colorBytes[i + 1] = uint8_t(kHalfTwoPointFive >> 8);
    }
    std::vector<float> depthBytes(size_t(w) * h, 0.5f);
    auto colorTex = texture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                            D3D11_BIND_SHADER_RESOURCE, colorBytes.data(), w * 8);
    auto depthTex = texture(device, w, h, DXGI_FORMAT_R32_FLOAT,
                            D3D11_BIND_SHADER_RESOURCE, depthBytes.data(), w * 4);
    auto colorView = view(device, colorTex.Get());
    auto depthView = view(device, depthTex.Get());
    if (!colorTex || !depthTex || !colorView || !depthView) return;
    edvr::FlatMonoResolveFrame f{};
    f.color = colorView.Get(); f.depth = depthView.Get();
    f.renderWidth = w; f.renderHeight = h; f.outputWidth = outW; f.outputHeight = outH;
    f.mode = FlatMonoResolveMode::Taa; f.fp16 = true; f.reset = true; f.frame = 70000;
    f.deltaMs = 16.667f;
    camera(f.camera); camera(f.previousCamera);
    ComPtr<ID3D11ShaderResourceView> output;
    const char* reason = "unset";
    expectedJx = f.jitterX; expectedJy = f.jitterY;
    check(edvr::flatMonoResolve(device, context, f, &output, &reason) && reason == nullptr && output,
          "copy route: an FP16 TAA frame below the output resolves");
    if (output) {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        output->GetDesc(&viewDesc);
        ComPtr<ID3D11Resource> resource;
        output->GetResource(&resource);
        ComPtr<ID3D11Texture2D> outTex;
        if (resource) resource.As(&outTex);
        D3D11_TEXTURE2D_DESC texDesc{};
        if (outTex) outTex->GetDesc(&texDesc);
        check(viewDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
              texDesc.Width == outW && texDesc.Height == outH,
              "copy route: the resolved output is FP16 at the evaluation size, for the game's own copy to sample");
        // Values above 1.0 survive: read the middle pixel back through staging.
        D3D11_TEXTURE2D_DESC stageDesc = texDesc;
        stageDesc.Usage = D3D11_USAGE_STAGING; stageDesc.BindFlags = stageDesc.MiscFlags = 0;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;
        uint16_t red = 0;
        if (outTex && SUCCEEDED(device->CreateTexture2D(&stageDesc, nullptr, &stage))) {
            context->CopyResource(stage.Get(), outTex.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                const auto* row = static_cast<const uint8_t*>(map.pData) + size_t(outH / 2) * map.RowPitch;
                uint16_t px = 0;
                std::memcpy(&px, row + size_t(outW / 2) * 8, sizeof(px));
                red = px;
                context->Unmap(stage.Get(), 0);
            }
        }
        check(red == kHalfTwoPointFive,
              "copy route: 2.5 in, 2.5 out -- no UNORM trip clamps the FP16 image (0x3C00 would be 1.0)");
    }
    // A second frame continues history in the same FP16 resources (no format-change reset).
    // A non-reset frame needs the engine source views (flatMonoResolve refuses
    // without them, whatever the backend): the key-off shape, invalid slots, a
    // zeroed pool and identity scene buffers -- the configuration the main
    // scenario resolves with before any engine record is written.
    std::vector<float> fp16Slots(size_t(w) * h * 2);
    for (size_t i = 0; i < fp16Slots.size(); i += 2) { fp16Slots[i] = -1.0f; fp16Slots[i + 1] = 0.01f; }
    auto fp16SlotTex = texture(device, w, h, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                               fp16Slots.data(), UINT(w) * 8);
    auto fp16SlotView = view(device, fp16SlotTex.Get());
    uint32_t fp16Record[84]{};
    D3D11_BUFFER_DESC fp16Bd{}; fp16Bd.ByteWidth = sizeof(fp16Record); fp16Bd.StructureByteStride = 336;
    fp16Bd.Usage = D3D11_USAGE_DEFAULT; fp16Bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    fp16Bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA fp16Initial{}; fp16Initial.pSysMem = fp16Record;
    ComPtr<ID3D11Buffer> fp16Pool;
    check(SUCCEEDED(device->CreateBuffer(&fp16Bd, &fp16Initial, fp16Pool.GetAddressOf())), "FP16 engine pool");
    auto fp16PoolView = view(device, fp16Pool.Get());
    float fp16Scene[277][4]{}; float fp16Cam[6][4]; camera(fp16Cam);
    std::memcpy(fp16Scene + 270, fp16Cam, sizeof(fp16Cam));
    // kFixtureStamp's value (flat_mono_resolve_test.cpp): the prep reads EN[276].x.
    { constexpr uint32_t kFp16EngineStamp = 77; std::memcpy(&fp16Scene[276][0], &kFp16EngineStamp, 4); }
    fp16Bd = {}; fp16Bd.ByteWidth = sizeof(fp16Scene); fp16Bd.Usage = D3D11_USAGE_DEFAULT;
    fp16Bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    fp16Initial.pSysMem = fp16Scene;
    ComPtr<ID3D11Buffer> fp16Now, fp16Old;
    check(SUCCEEDED(device->CreateBuffer(&fp16Bd, &fp16Initial, fp16Now.GetAddressOf())) &&
          SUCCEEDED(device->CreateBuffer(&fp16Bd, &fp16Initial, fp16Old.GetAddressOf())), "FP16 engine camera buffers");
    f.reset = false; f.frame = 70001;
    f.engine = {fp16SlotView.Get(), fp16PoolView.Get(), fp16Now.Get(), fp16Old.Get()};
    output.Reset(); reason = "unset";
    expectedJx = f.jitterX; expectedJy = f.jitterY;
    check(edvr::flatMonoResolve(device, context, f, &output, &reason) && reason == nullptr && output,
          "copy route: the next FP16 frame continues, same resources, no reallocation refusal");
    edvr::flatMonoResolveReset();
}
