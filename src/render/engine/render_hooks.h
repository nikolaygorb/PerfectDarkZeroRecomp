#pragma once

#include <rex/ppc/context.h>

namespace pdz::engine {
// The project already hooks Swap for cutscene pacing; that hook calls this
// instead of a second strong PPC symbol.
void OnSwap(PPCContext& ctx, uint8_t* base);
}
