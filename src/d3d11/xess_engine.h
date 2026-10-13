#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

// XeSS-SR keeps one feature per upscaler slot (dlaa.h, kUpscalerSlots); each
// slot is keyed and rebuilt on its own. The flat resolver uses slot 0. This
// backend uses Intel's D3D12 SDK over a shared D3D11/D3D12 device pair; it does
// not provide frame generation.
bool xessBuiltIn();
bool xessAvailable(ID3D11Device* device, const char** why);

// Colour, motion, and depth are the flat resolver's render-grid textures;
// output is its display-grid texture. Motion is current -> previous in input
// pixels. Jitter is the positive-right/down projection phase in input pixels.
// On HDR frames colour is R11G11B10_FLOAT and output is R16G16B16A16_FLOAT;
// XeSS receives matched R11 input/output and a precompiled compute copy widens
// the result. LDR uses matched R8G8B8A8_UNORM directly.
bool xessEvaluate(ID3D11DeviceContext* context, unsigned slot,
                  ID3D11Texture2D* colour, ID3D11Texture2D* depth,
                  ID3D11Texture2D* motion, ID3D11Texture2D* output,
                  uint32_t width, uint32_t height, uint32_t outputWidth,
                  uint32_t outputHeight, float jitterX, float jitterY,
                  bool reset, bool hdr, const char** why);

// Releases every slot's feature (each with its bounded GPU wait); the library
// and the D3D12 device stay loaded. Safe when XeSS never started.
void xessReleaseFeatures();
void xessShutdown();

// Rig seams (fsr3_engine.h's fsr3TestContextCreations pattern): features a slot
// has built since the backend started, and whether a slot holds one now.
uint32_t xessTestFeatureCreations(unsigned slot);
bool xessTestFeatureLive(unsigned slot);

// The slot's measured price, as the temporal pass's totals line reads it: evaluations, resets, and the mean and
// maximum CPU milliseconds per successful evaluate. False until the slot has evaluated, and in a build without the SDK.
bool xessSlotTotals(unsigned slot, uint32_t* evaluations, uint32_t* resets, double* avgMs, double* maxMs);

}  // namespace edvr
