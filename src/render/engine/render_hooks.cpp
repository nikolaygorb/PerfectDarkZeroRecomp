#include "render_hooks.h"
#include "scene_probe.h"

#include <rex/hook.h>

REX_EXTERN(__imp__sub_82611C70);  // Composition
REX_EXTERN(__imp__sub_826CFC10);  // Swap (hooked by timing/cutscene_frame_cap_hook.cpp)
REX_EXTERN(__imp__sub_826D43B8);  // CreateVS
REX_EXTERN(__imp__sub_826DA198);  // Draw
REX_EXTERN(__imp__sub_826DA3D8);  // DrawIndexed
REX_EXTERN(__imp__sub_826E2480);  // Resolve

namespace scene = pdz::engine::scene;

// Composition: scene end without a configured resolve (fallback), post end after.
REX_HOOK_RAW(sub_82611C70) {
  const bool streaming = scene::Streaming();
  if (streaming) scene::Composition(false);
  __imp__sub_82611C70(ctx, base);
  if (streaming) scene::Composition(true);
}

// CreateVS(source): the returned object identifies the world view shader.
REX_HOOK_RAW(sub_826D43B8) {
  const uint32_t source = ctx.r3.u32;
  __imp__sub_826D43B8(ctx, base);
  if (scene::Enabled()) scene::OnCreateVS(source, ctx.r3.u32);
}

// Draw / DrawIndexed(device, ...): the camera marker follows the first world view draw.
REX_HOOK_RAW(sub_826DA198) {
  const uint32_t device = ctx.r3.u32;
  __imp__sub_826DA198(ctx, base);
  if (scene::Streaming()) scene::AfterDraw(device);
}
REX_HOOK_RAW(sub_826DA3D8) {
  const uint32_t device = ctx.r3.u32;
  __imp__sub_826DA3D8(ctx, base);
  if (scene::Streaming()) scene::AfterDraw(device);
}

// Resolve(device, flags): scene end before the configured caller's resolve.
REX_HOOK_RAW(sub_826E2480) {
  if (scene::Streaming()) scene::BeforeResolve(ctx.r3.u32, uint32_t(ctx.lr), ctx.r4.u32);
  __imp__sub_826E2480(ctx, base);
}

void pdz::engine::OnSwap(PPCContext& ctx, uint8_t* base) {
  const uint32_t device = ctx.r3.u32;
  const bool streaming = scene::Streaming();
  if (streaming) scene::BeforeSwap(device);
  __imp__sub_826CFC10(ctx, base);
  if (scene::Enabled()) scene::OnSwap();
}
