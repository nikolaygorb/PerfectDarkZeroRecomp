// game_cvars.h - Game-specific CVAR declarations
#pragma once

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, graphics_backend);
REXCVAR_DECLARE(bool, pdz_fps60_unlock);
REXCVAR_DECLARE(bool, pdz_aspect_ratio_16_9);
REXCVAR_DECLARE(bool, dev_debug_runtime);
