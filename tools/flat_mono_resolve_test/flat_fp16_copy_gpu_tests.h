// The copy route under an FP16 presentation (EDHDR's R16G16B16A16_FLOAT
// swapchain) on WARP: the structure admits the FP16 source (flat_copy_structure.h),
// the preflight accepts the FP16 view and allocates FP16 resources, and EDVR's
// own TAA -- which needs no SDK -- resolves a render-below-output frame and
// hands back an FP16 output with values above 1.0 intact (a UNORM trip would
// clamp them to 1.0). Also checks direct FP16 input for MFX3/4 against the rig
// backend seam; no native MetalFX implementation is required on Windows.
#pragma once
#include "../../src/d3d11/flat_diagnostic.h"

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
    // MetalFX now reads the original FP16 view in prep/finish. Poison the
    // private copy through the preceding TAA frames, then change the input on
    // continuation: stale private-color use must fail the complete-image check.
    std::vector<uint8_t> coverage(size_t(w)*h);
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w/2;++x)coverage[size_t(y)*w+x]=255;
    auto coverageTex=texture(device,w,h,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE,coverage.data(),w);
    auto coverageView=view(device,coverageTex.Get());f.untrustedCameraCoverage=coverageView.Get();
    captureMfxColor=true;
    for(auto mode : {FlatMonoResolveMode::Mfx,FlatMonoResolveMode::Mfx4}) {
        f.mode=mode;f.frame+=100;f.jitterX=.25f;f.jitterY=-.125f;
        f.previousJitterX=f.jitterX;f.previousJitterY=f.jitterY;
        for(unsigned step=0;step<2;++step) {
            const uint16_t value=step?0x4300:0x4100; // 3.5 / 2.5, both above 1
            for(size_t i=0;i<colorBytes.size();i+=2)std::memcpy(colorBytes.data()+i,&value,2);
            context->UpdateSubresource(colorTex.Get(),0,nullptr,colorBytes.data(),w*8,0);
            f.reset=step==0;++f.frame;output.Reset();reason="unset";
            expectedJx=f.jitterX;expectedJy=f.jitterY;
            const int calls=mfxCalls;
            check(edvr::flatMonoResolve(device,context,f,&output,&reason) && output && !reason,
                  "FP16 MetalFX reset and continuation resolve with jitter and mixed rejection");
            check(mfxCalls==calls+1 && backendReset==(step==0) &&
                  observedMfxGeneration==(mode==FlatMonoResolveMode::Mfx?edvr::mfx::FxGeneration::Metal3:edvr::mfx::FxGeneration::Metal4),
                  "FP16 MetalFX generation, one backend call and history reset preserved");
            bool equal=observedMfxColor.size()==colorBytes.size();
            for(size_t i=0;equal && i<observedMfxColor.size();i+=2) {
                uint16_t v=0;std::memcpy(&v,observedMfxColor.data()+i,2);
                equal &= v==((i%8==6)?0x3c00:value);
            }
            check(equal && observedMfxColorResource!=colorTex.Get(),
                  "complete MetalFX input has unchanged FP16 RGB, alpha one, and private storage");
            ComPtr<ID3D11Resource> outputResource;if(output)output->GetResource(&outputResource);
            ComPtr<ID3D11Texture2D> outputTex;if(outputResource)outputResource.As(&outputTex);
            D3D11_TEXTURE2D_DESC outputDesc{};if(outputTex)outputTex->GetDesc(&outputDesc);
            check(outputTex && outputDesc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT &&
                  outputDesc.Width==outW && outputDesc.Height==outH && outputResource.Get()!=colorTex.Get(),
                  "MetalFX finish returns a private FP16 output at the evaluation size");
            std::vector<unsigned char> result,source;
            check(readWhole(context,outputTex.Get(),result,8) && result.size()==size_t(outW)*outH*8,
                  "complete MetalFX output is readable at the expected size");
            if(result.size()==size_t(outW)*outH*8) {
                uint16_t rejected=0,accepted=0;
                std::memcpy(&rejected,result.data()+(size_t(outH/2)*outW+outW/4)*8,2);
                std::memcpy(&accepted,result.data()+(size_t(outH/2)*outW+3*outW/4)*8+2,2);
                check(rejected==value && accepted==(step?0x3c00:value),
                      "rejected pixels retain HDR values from original, trusted pixels use backend green");
            }
            check(readWhole(context,colorTex.Get(),source,8) && source==colorBytes,
                  "MetalFX prep and finish leave every source FP16 component unchanged");
        }
    }
    captureMfxColor=false;
    // Real resolver execution for every independent selector combination. The rig's SDK seam
    // records calls and reads inputs; Windows runs the production shaders/restore guard on WARP.
    for(const auto mode:{FlatMonoResolveMode::Taa,FlatMonoResolveMode::Fsr,FlatMonoResolveMode::Mfx,FlatMonoResolveMode::Mfx4}) {
        edvr::flatMonoResolveReset();
        f.mode=mode;f.frame=90000;f.jitterX=f.jitterY=f.previousJitterX=f.previousJitterY=0;
        f.engine={};f.reset=false;f.steadyDetail=true;expectedJx=expectedJy=0;expectedNear=f.camera[3][2];
        for(unsigned bits=0;bits<16;++bits) {
            edvr::flatdiag::publish(edvr::flatdiag::selection(true,!(bits&1),!(bits&2),!(bits&4),!(bits&8)));
            edvr::flatdiag::clearCounts();
            const int calls=backendCalls;
            for(unsigned step=0;step<2;++step) {
                ++f.frame;output.Reset();reason="unset";
                check(edvr::flatMonoResolve(device,context,f,&output,&reason) && output && !reason,
                      "diagnostic FP16 combinations keep the actual resolver active");
                const bool backendOn=!(bits&8);
                check(backendCalls==calls+(mode!=FlatMonoResolveMode::Taa && backendOn?int(step+1):0),
                      "diagnostic backend selection matches actual SDK calls without disabling prep");
                if(mode!=FlatMonoResolveMode::Taa && backendOn)check(backendReset,"diagnostic SDK history reset on every frame");
                ComPtr<ID3D11Resource> resource;if(output)output->GetResource(&resource);
                ComPtr<ID3D11Texture2D> tex;if(resource)resource.As(&tex);
                uint16_t red=0;
                check(tex && readPixel(context,tex.Get(),&red,sizeof(red),outW/2,outH/2) && red==0x4300,
                      "diagnostic output retains current-frame FP16 HDR 3.5, including spatial bypass");
                std::vector<unsigned char> unchanged;
                check(readWhole(context,colorTex.Get(),unchanged,8) && unchanged==colorBytes,
                      "diagnostic bypass never overwrites original FP16 source");
            }
            const auto* counts=edvr::flatdiag::counts;
            check(counts[edvr::flatdiag::Resolve]==2 && counts[edvr::flatdiag::Finished]==2 &&
                  counts[edvr::flatdiag::Prepared]==((bits&2)?0u:2u) &&
                  counts[edvr::flatdiag::Evaluated]==((bits&8)?0u:2u) &&
                  counts[edvr::flatdiag::Spatial]==((bits&8)?2u:0u),
                  "diagnostic counters prove actual prep/evaluation/spatial independence");
            check(counts[edvr::flatdiag::Neutral]<=1,"prepared input seed never repeats each frame");
        }
        // Deliberately cold preparation-off: no previous output may be reused uninitialized.
        edvr::flatMonoResolveReset();edvr::flatdiag::publish(edvr::flatdiag::selection(true,true,false,true,true));
        ++f.frame;output.Reset();reason="unset";
        check(edvr::flatMonoResolve(device,context,f,&output,&reason) && output && !reason,
              "cold preparation bypass initializes finite inputs without a preceding prepared frame");
        if(mode!=FlatMonoResolveMode::Taa)check(std::isfinite(observedDepth) && observedDepth==.5f &&
            observedMotion==0 && observedMotionY==0,"cold backend depth/motion initialized to finite neutral values");
    }
    // A warmed freeze must use the depth plane most recently written, not an allocated but stale sibling.
    edvr::flatMonoResolveReset();f.mode=FlatMonoResolveMode::Mfx;f.steadyDetail=true;
    edvr::flatdiag::publish(edvr::flatdiag::selection(true,true,true,true,true));
    ++f.frame;output.Reset();check(edvr::flatMonoResolve(device,context,f,&output,&reason),"warm prepared input before freeze");
    std::fill(depthBytes.begin(),depthBytes.end(),.25f);
    context->UpdateSubresource(depthTex.Get(),0,nullptr,depthBytes.data(),w*4,0);
    const uint16_t fresh=0x4100;for(size_t i=0;i<colorBytes.size();i+=2)std::memcpy(colorBytes.data()+i,&fresh,2);
    context->UpdateSubresource(colorTex.Get(),0,nullptr,colorBytes.data(),w*8,0);
    edvr::flatdiag::publish(edvr::flatdiag::selection(true,true,false,true,true));
    captureMfxColor=true;edvr::flatdiag::clearCounts();
    for(unsigned step=0;step<2;++step) {
        ++f.frame;output.Reset();reason="unset";
        check(edvr::flatMonoResolve(device,context,f,&output,&reason) && output,"freeze resolves after scene changes");
        uint16_t seed=0;if(observedMfxColor.size()>=2)std::memcpy(&seed,observedMfxColor.data(),2);
        check(observedDepth==.5f && seed==0x4300,"frozen backend reads previously prepared depth and complex colour, not current or undefined data");
        ComPtr<ID3D11Resource> resource;if(output)output->GetResource(&resource);
        ComPtr<ID3D11Texture2D> tex;if(resource)resource.As(&tex);uint16_t red=0;
        check(tex && readPixel(context,tex.Get(),&red,2,outW/2,outH/2) && red==fresh,"freeze finish still displays the current game image");
    }
    check(edvr::flatdiag::counts[edvr::flatdiag::Neutral]==1,"freeze initialized once, not every resolve");
    captureMfxColor=false;
    edvr::flatdiag::publish(0);edvr::flatdiag::clearCounts();
    f.engine={fp16SlotView.Get(),fp16PoolView.Get(),fp16Now.Get(),fp16Old.Get()};
    for(unsigned step=0;step<2;++step) {
        ++f.frame;output.Reset();reason="unset";
        check(edvr::flatMonoResolve(device,context,f,&output,&reason) && output && backendReset==(step==0),
              "leaving diagnostics resets once, then resumes normal MetalFX history");
    }
    captureMfxColor=false;observedMfxColor.clear();observedMfxColorResource=nullptr;
    edvr::flatMonoResolveReset();
}
