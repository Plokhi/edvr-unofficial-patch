#pragma once

// SMAA's two lookup textures (MIT, src/d3d11/smaa/Textures, AreaTex and SearchTex, unmodified). map_smaa.cpp holds the arrays, so the
// resolver does not compile 1.1 MB of initialiser text. The bytes are SMAA's own layout: AreaTex is R8G8 (160 x 560), SearchTex is R8
// (64 x 16), each row pitch bytes wide.

#include <cstdint>

namespace edvr {

struct MapSmaaTable {
    const unsigned char* bytes;
    uint32_t width, height, pitch;
};

MapSmaaTable mapSmaaAreaTable();    // R8G8_UNORM
MapSmaaTable mapSmaaSearchTable();  // R8_UNORM

}  // namespace edvr
