// SMAA's lookup textures for the map temporal-off frames' SMAA 1x (map_smaa.h). The two headers are SMAA's own, unmodified (src/d3d11/smaa/
// README.md, MIT). They are included here and nowhere else, so their arrays exist once.

#include "map_smaa.h"

#include "smaa/Textures/AreaTex.h"
#include "smaa/Textures/SearchTex.h"

static_assert(AREATEX_WIDTH == 160 && AREATEX_HEIGHT == 560 && AREATEX_PITCH == 320, "SMAA AreaTex layout (R8G8, 160 x 560)");
static_assert(SEARCHTEX_WIDTH == 64 && SEARCHTEX_HEIGHT == 16 && SEARCHTEX_PITCH == 64, "SMAA SearchTex layout (R8, 64 x 16)");

namespace edvr {

MapSmaaTable mapSmaaAreaTable() {
    return MapSmaaTable{areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT, AREATEX_PITCH};
}

MapSmaaTable mapSmaaSearchTable() {
    return MapSmaaTable{searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, SEARCHTEX_PITCH};
}

}  // namespace edvr
