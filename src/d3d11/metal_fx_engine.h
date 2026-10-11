// Apple MetalFX at the door -- the MetalFX temporal scaler, invoked natively
// by DXMT (CodeWeavers' D3D11-over-Metal) through its own private extension
// interface, so fix.temporal_aa = mfx hands it the same canonical inputs
// dlaaEvaluate and fsr3Evaluate are handed and MetalFX writes into the same
// output texture. Nothing here is a Metal call: no MetalFX header, no MetalFX
// symbol, no nvngx, no NVAPI, no DXMT_ENABLE_NVEXT.
//
// The division of labour is the point, and it is the reason this file is
// small. EDVR owns the Elite-specific temporal semantics -- which texture is
// the scene, which is depth, which is motion, what the jitter and reset are,
// which sizes, and where the result is presented -- all of which the working
// frontend already produces and none of which MetalFX has any opinion about.
// DXMT owns the native MetalFX invocation, because DXMT is the only layer that
// can: it holds the MTLTexture objects behind these same D3D11 resources, it
// owns the command buffer the encode has to be ordered into, and it owns the
// hazard and residency tracking that keeps the encode correctly ordered
// against the D3D11 work that produced the inputs. Its TemporalUpscale does
// the resource translation with no copy: the MTLTexture the scaler reads is
// the allocation EDVR's UAVs already wrote.
//
// The interface is DXMT's, not a public D3D11 one, so this file declares the
// ABI it needs rather than including anything of DXMT's (EDVR never includes
// DXMT headers; DXMT is read-only reference). The authoritative reference is
// dxmt/src/d3d11/d3d11_interfaces.hpp at the commits this was written against
// (db83f85 for Ext/Ext1, 661c77d for Ext2), and the static_asserts below pin
// the layout those declarations produce. They catch a layout EDVR gets wrong,
// not one DXMT changes later: tools\check_metal_fx_backend.py re-derives the
// field offsets and the three IIDs from this file and, when a DXMT checkout is
// beside the tree, from DXMT's own header, so a divergence between the two
// fails the build.
//
// What is NOT here, deliberately:
//   - The macOS 27 temporal-scaler options this backend could want (reactive
//     mask, jittered motion vectors, output-resolution motion vectors) as
//     anything but a note. They are all on the MetalFX 3 descriptor, which
//     remains the single descriptor below: MetalFX 4 is not a different
//     algorithm, MTL4FXTemporalScaler refines MTLFXTemporalScalerBase, so
//     every field means exactly what it already meant. Selecting the
//     generation therefore adds no input, no size, no format, no motion
//     vector and no reset -- see mfxEvaluate's `generation`.
//   - A reactive mask. MetalFX accepts one and EDVR's rejection mask is the
//     right shape (R8_UNORM), but DXMT's descriptor has no field for it and
//     DXMT is read-only for this milestone. EDVR's existing finish kernel
//     already consumes the rejection mask for every trained backend, so
//     passing nothing here loses nothing today.
//   - A history texture. MetalFX owns its temporal history inside the scaler
//     instance DXMT caches, exactly as it does behind nvngx. EDVR's own
//     reset() result is handed over and nothing else is.
//
// JitterOffsetX/Y retain the +1.0 sign verified by earlier MetalFX 3 flights.
// Both generations share that phase. Runtime validation of the v0.19.1 port
// remains outstanding; the descriptor alone does not prove the picture.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unknwn.h>
#include <windef.h>

struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11Texture2D;

namespace edvr {
namespace mfx {

// ---- DXMT's private ABI, declared here rather than included -------------------------------------------
// IMTLD3D11ContextExt  {43ace3ce-1956-448b-a4eb-aee68bdeb283}
// IMTLD3D11ContextExt1 {19a8e35a-38be-418f-94e3-9f7323936870}
// (dxmt/src/d3d11/d3d11_interfaces.hpp:24 and :36). Spelled as fields, not
// __declspec(uuid(...)), so the constants are the same value in every
// compiler EDVR is built with and can be static_asserted below. The reference
// build encodes them from the same strings with dxmt::guid::make_guid; the
// GUID value is what QueryInterface compares, and these are those values.
constexpr GUID kIidContextExt = {0x43ace3ceu, 0x1956u, 0x448bu,
                                 {0xa4, 0xeb, 0xae, 0xe6, 0x8b, 0xde, 0xb2, 0x83}};
constexpr GUID kIidContextExt1 = {0x19a8e35au, 0x38beu, 0x418fu,
                                  {0x94, 0xe3, 0x9f, 0x73, 0x23, 0x93, 0x68, 0x70}};
// IMTLD3D11ContextExt2 {6523d273-9915-44b3-a5f0-f69a654da1f8}
// (dxmt/src/d3d11/d3d11_interfaces.hpp:65, at 661c77d). DXMT's own revision of
// the extension rather than a new descriptor: TemporalUpscaleEx takes the same
// MTL_TEMPORAL_UPSCALE_D3D11_DESC and adds the generation as a separate
// argument, so the 88-byte struct below stays byte-for-byte what it was and an
// older caller's allocation is never read past. EDVR follows that: it does not
// declare a second descriptor, and does not need one.
constexpr GUID kIidContextExt2 = {0x6523d273u, 0x9915u, 0x44b3u,
                                  {0xa5, 0xf0, 0xf6, 0x9a, 0x65, 0x4d, 0xa1, 0xf8}};
// A GUID built from the two strings above, re-derived at compile time, so a
// typo in either constant is a build failure and not a QueryInterface that
// quietly returns E_NOINTERFACE at runtime.
static_assert(kIidContextExt.Data1 == 0x43ace3ceu && kIidContextExt.Data2 == 0x1956u &&
                  kIidContextExt.Data3 == 0x448bu && kIidContextExt.Data4[7] == 0x83u &&
                  kIidContextExt.Data4[0] == 0xa4u && kIidContextExt.Data4[1] == 0xebu,
              "kIidContextExt must be 43ace3ce-1956-448b-a4eb-aee68bdeb283");
static_assert(kIidContextExt1.Data1 == 0x19a8e35au && kIidContextExt1.Data2 == 0x38beu &&
                  kIidContextExt1.Data3 == 0x418fu && kIidContextExt1.Data4[7] == 0x70u &&
                  kIidContextExt1.Data4[0] == 0x94u && kIidContextExt1.Data4[1] == 0xe3u,
              "kIidContextExt1 must be 19a8e35a-38be-418f-94e3-9f7323936870");
static_assert(kIidContextExt2.Data1 == 0x6523d273u && kIidContextExt2.Data2 == 0x9915u &&
                  kIidContextExt2.Data3 == 0x44b3u && kIidContextExt2.Data4[7] == 0xf8u &&
                  kIidContextExt2.Data4[0] == 0xa5u && kIidContextExt2.Data4[1] == 0xf0u &&
                  kIidContextExt2.Data4[5] == 0x4du && kIidContextExt2.Data4[6] == 0xa1u,
              "kIidContextExt2 must be 6523d273-9915-44b3-a5f0-f69a654da1f8");

// dxmt/src/d3d11/d3d11_interfaces.hpp:5. Field order is DXMT's, verbatim: a
// vtable of this struct crossing into DXMT is a memcpy of the whole thing
// field for field, so the order below is not EDVR's to choose.
struct TemporalUpscaleDesc {
    UINT InputContentWidth;          //  0; 0 means full width
    UINT InputContentHeight;         //  4; 0 means full height
    BOOL AutoExposure;               //  8
    BOOL InReset;                    // 12
    BOOL DepthReversed;              // 16
    BOOL MotionVectorInDisplayRes;   // 20
    ID3D11Resource *Color;           // 24 (the first 8-byte-aligned field)
    ID3D11Resource *Depth;           // 32
    ID3D11Resource *MotionVector;    // 40
    ID3D11Resource *Output;          // 48
    FLOAT MotionVectorScaleX;        // 56
    FLOAT MotionVectorScaleY;        // 60
    FLOAT PreExposure;               // 64
    ID3D11Resource *ExposureTexture; // 72 (four bytes of padding after PreExposure)
    FLOAT JitterOffsetX;             // 80
    FLOAT JitterOffsetY;             // 84
};

// 88 bytes on x64, with the padding DXMT's field order forces before
// ExposureTexture. A change here is an ABI change, and must not be a silent
// one: MetalFX would read whatever EDVR left in those four bytes.
static_assert(sizeof(void *) == 8, "the offsets below are x64's");
static_assert(sizeof(TemporalUpscaleDesc) == 88, "MTL_TEMPORAL_UPSCALE_D3D11_DESC must be 88 bytes on x64");
static_assert(offsetof(TemporalUpscaleDesc, InputContentWidth) == 0, "");
static_assert(offsetof(TemporalUpscaleDesc, InputContentHeight) == 4, "");
static_assert(offsetof(TemporalUpscaleDesc, AutoExposure) == 8, "");
static_assert(offsetof(TemporalUpscaleDesc, InReset) == 12, "");
static_assert(offsetof(TemporalUpscaleDesc, DepthReversed) == 16, "");
static_assert(offsetof(TemporalUpscaleDesc, MotionVectorInDisplayRes) == 20, "");
static_assert(offsetof(TemporalUpscaleDesc, Color) == 24, "");
static_assert(offsetof(TemporalUpscaleDesc, Depth) == 32, "");
static_assert(offsetof(TemporalUpscaleDesc, MotionVector) == 40, "");
static_assert(offsetof(TemporalUpscaleDesc, Output) == 48, "");
static_assert(offsetof(TemporalUpscaleDesc, MotionVectorScaleX) == 56, "");
static_assert(offsetof(TemporalUpscaleDesc, MotionVectorScaleY) == 60, "");
static_assert(offsetof(TemporalUpscaleDesc, PreExposure) == 64, "");
static_assert(offsetof(TemporalUpscaleDesc, ExposureTexture) == 72, "");
static_assert(offsetof(TemporalUpscaleDesc, JitterOffsetX) == 80, "");
static_assert(offsetof(TemporalUpscaleDesc, JitterOffsetY) == 84, "");
static_assert(sizeof(UINT) == 4 && sizeof(BOOL) == 4 && sizeof(FLOAT) == 4, "");
// UINT must be unsigned, not a typedef that MSVC could size differently: the
// desc is read by DXMT as the fields it declared, not as these.
static_assert((std::is_unsigned<UINT>::value) && sizeof(UINT) == 4, "UINT must be a 4-byte unsigned");
// And the descriptor must not be given any implicit padding of its own beyond
// what the declared order already forces: a memcmp against DXMT's struct is
// then only meaningful field for field, which is how the guard reads it.
static_assert(sizeof(TemporalUpscaleDesc) % alignof(TemporalUpscaleDesc) == 0, "no tail padding");

// dxmt/src/d3d11/d3d11_interfaces.hpp:32, :46. Two values, and the second is
// additive: the first is the MetalFX 3 answer EDVR has always asked, the
// second whether this GPU also supports temporal scaling through Metal 4
// (id<MTL4FXTemporalScaler>, which encodes into an id<MTL4CommandBuffer>).
// DXMT answers the second with FALSE rather than an error when the host is
// older than macOS 26, so a FALSE here is a normal outcome and not a fault.
enum class Feature : int { MetalFxTemporalScaler = 0, MetalFxTemporalScalerMetal4 = 1 };

// dxmt/src/d3d11/d3d11_interfaces.hpp:39, an unscoped enum with UINT as its
// fixed underlying type -- which is why this is UINT and not the int Feature
// above: DXMT reads the argument as that type, and the widths must match.
// MTL_FX_GENERATION_METAL3 is 0, so an unspecified request means Metal 3.
enum class FxGeneration : UINT { Metal3 = 0u, Metal4 = 1u };
static_assert(sizeof(FxGeneration) == 4, "MTL_FX_GENERATION's underlying type is UINT");
static_assert(static_cast<UINT>(FxGeneration::Metal3) == 0u, "");
static_assert(static_cast<UINT>(FxGeneration::Metal4) == 1u, "");
// Crossed into DXMT as a value, not as a pointer to one of these.
static_assert(std::is_trivially_copyable<FxGeneration>::value, "");

// dxmt/src/d3d11/d3d11_interfaces.hpp:24. The base is IUnknown with DXMT's
// three methods in DXMT's order, so TemporalUpscale is vtable slot 3 and
// CheckFeatureSupport slot 6 of IMTLD3D11ContextExt1.
struct ContextExt : public IUnknown {
    virtual void STDMETHODCALLTYPE TemporalUpscale(const TemporalUpscaleDesc *pDesc) = 0;
    virtual void STDMETHODCALLTYPE BeginUAVOverlap() = 0;
    virtual void STDMETHODCALLTYPE EndUAVOverlap() = 0;
};
struct ContextExt1 : public ContextExt {
    virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(Feature feature, void *pFeatureSupportData,
                                                          UINT featureSupportDataSize) = 0;
};
// dxmt/src/d3d11/d3d11_interfaces.hpp:65. Exactly one more virtual, so
// TemporalUpscaleEx is vtable slot 7 of IMTLD3D11ContextExt2 (IUnknown's three,
// Ext's three, Ext1's one), and Ext2 is strictly an extension of Ext1 -- an
// Ext2 pointer is usable wherever an Ext1 pointer is. That is why mfxEvaluate
// keeps asking the generation-neutral Ext1 question and reaches for Ext2 only on
// the Metal 4 call.
//
// The slot numbers are the whole ABI risk of this chain, and they cannot be
// asserted here: sizeof() of an interface with no data members is one vtable
// pointer under MSVC and under clang, whatever the vtable holds, so a size
// assertion says nothing about which slot a method occupies and would pass on a
// declaration order that is completely wrong. What pins the order instead is
// that these three declarations are DXMT's, in DXMT's order, verbatim --
// checked against DXMT's own header by tools\check_metal_fx_backend.py, which
// fails the build if the two ever diverge.
struct ContextExt2 : public ContextExt1 {
    virtual void STDMETHODCALLTYPE TemporalUpscaleEx(const TemporalUpscaleDesc *pDesc,
                                                     FxGeneration generation) = 0;
};

} // namespace mfx

// Is MetalFX temporal scaling usable from this context? Asks the live DXMT
// extension interface -- not a guess from a driver or a module name -- and
// answers false with a stable reason string for the log when it is not:
//   "flat-mfx-no-dxmt-context-ext"        the context is not DXMT's, or the QI failed
//   "flat-mfx-no-dxmt-context-ext1"       the older interface, so no feature query
//   "flat-mfx-check-feature-support-failed" DXMT's own query refused
//   "flat-mfx-unsupported"                DXMT answered: MetalFX temporal scaling is unavailable
//   "flat-mfx-no-context"                 no context was handed
// Does not need DXMT_ENABLE_NVEXT, nvngx.dll or any NVIDIA registry key: this
// is DXMT's own interface, and the NVEXT gate that nvngx.dll needs governs
// only the vendor-extension entry point, not this one. Cheap after the first
// call; the extension pointer is cached per context.
bool mfxAvailable(ID3D11DeviceContext* ctx, const char** why);

// Is METALFX 4 -- fix.temporal_aa = mfx4 -- usable from this context? Two
// questions, both required, in this order, and neither is allowed to pass
// silently for the other:
//   1. IMTLD3D11ContextExt2. Without it DXMT has no TemporalUpscaleEx at all,
//      so there is nothing to call for the Metal 4 generation.
//   2. MTL_FEATURE_METALFX_TEMPORAL_SCALER_METAL4, which is what says this GPU
//      can scale through Metal 4. Querying it does not select Metal 4; it only
//      says whether selecting it is worth attempting.
// This NEVER reports the Metal 3 answer as its own. mfx (MetalFX 3) and mfx4
// are separate requests and a caller that asked for Metal 4 is told no rather
// than quietly given Metal 3: DXMT has no silent fallback either, and a
// fallback here would make the in-game A/B meaningless -- the whole point of
// the two modes is that each frame ran the generation its mode named.
// Reasons, all "flat-mfx4-*", so a log line's prefix alone says which
// generation was asked for and where it stopped.
bool mfxAvailableEx(ID3D11DeviceContext* ctx, const char** why);

// One frame. colour is fp16 RGBA at w x h (the canonical MetalFX colour
// format), depth r32f and mv rg16f at the same size, out fp16 RGBA at
// outW x outH -- all four are ordinary D3D11 textures with SRV and, where the
// frontend writes them, UAV, and DXMT hands MetalFX the MTLTexture objects
// that already back them, with no copy in either direction. jx/jy are
// EDVR's current-frame raster phase in render pixels; reset is the frontend's
// effective temporal reset (the same value FSR3 and NGX are given), not an
// MFX-specific one. autoExposure is the HDR route's flag. False on any
// refusal, with why.
//
// THE JITTER SIGN, ANSWERED BY FLIGHT. EDVR's phase is positive right/down in
// [-0.5, 0.5) (src/common/temporal_math.h:90); MetalFX's jitterOffset is
// documented only as "the pixel offset this scaler samples to return to the
// frame's reference frame", so the sign could not be read out of either header.
// The first M5 Pro flight ran with kJitterSign = -1 -- the negation, chosen on
// the reasoning that DXMT passes DLSS's jitterOffset to this same scaler and so
// DLSS's convention was the intended one.
//
// That was an inference, and the flight refuted it. DXMT answered ContextExt1,
// MetalFX ran and treated frames continuously, and the presented image visibly
// RETAINED the projection jitter -- the scaler was handed an offset that undid
// nothing, or the wrong amount of it, so the jitter stayed in the picture
// instead of averaging away. MetalFX wants the phase EDVR has already computed,
// handed over as it stands.
//
// This is the whole A/B. JitterOffsetX/Y below are kJitterSign times the phase,
// so flipping this one constant flips both axes and moves nothing else in the
// backend: no input, no size, no format, no motion vector, no reset.
constexpr float kJitterSign = 1.0f;
inline float mfxJitterOffsetX(float jx) { return kJitterSign * jx; }
inline float mfxJitterOffsetY(float jy) { return kJitterSign * jy; }

// generation selects WHICH scaler runs, and is the only difference between
// fix.temporal_aa = mfx and fix.temporal_aa = mfx4. Everything below it -- the
// descriptor's fields, the formats, the sizes, the jitter sign, the reset, the
// output texture -- is computed once and is identical for both, which is the
// point: MTL4FXTemporalScaler refines MTLFXTemporalScalerBase, so the two
// generations consume the same inputs and EDVR's temporal semantics carry over
// untouched. MetalFX 4 is reached by calling DXMT's Ext2 TemporalUpscaleEx
// instead of Ext1's TemporalUpscale, and by nothing else.
//
// FxGeneration::Metal3 calls the ORIGINAL TemporalUpscale on the ORIGINAL
// Ext1 interface, byte for byte as it always has. FxGeneration::Metal4 calls
// only TemporalUpscaleEx, and only after Ext2 and the Metal 4 feature have both
// answered yes. There is no path from Metal4 to TemporalUpscale: if Ext2 is
// missing or the feature is unsupported this returns false with a "flat-mfx4-*"
// reason and no descriptor crosses into DXMT at all.
// TemporalUpscale returns void: a dispatched call proves EDVR asked, not that
// DXMT created the scaler or that the GPU ran it (its implementation returns
// silently on bad inputs). The backend refuses everything it can see -- null
// resources, zero extents, missing interface, refused feature -- but absence
// of failure is not success; the flight judges the picture.

bool mfxEvaluate(ID3D11DeviceContext* ctx, ID3D11Texture2D* colour, ID3D11Texture2D* depth,
                 ID3D11Texture2D* mv, ID3D11Texture2D* out, uint32_t w, uint32_t h, uint32_t outW,
                 uint32_t outH, float jx, float jy, bool reset, const char** why, bool autoExposure,
                 mfx::FxGeneration generation);

} // namespace edvr