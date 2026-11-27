#pragma once
#include <cstdint>
#include <filesystem>
namespace rex { class Runtime; }
// PDZ scene producer: annotates the title's command stream with SDK scene
// markers (rex/graphics/title_scene.h). All PDZ knowledge of the camera,
// the world shader and the composition boundary lives here.
namespace pdz::engine::scene {
// Starts tracking when the GPU plugin takes scene markers. The stream runs
// while the SDK's consumer is on (a temporal upscaler, native_scene_frames,
// pdz_engine_scene_stream), switched at the next frame, or for a capture.
// Empty directory: no diagnostic files.
void Initialize(rex::Runtime& runtime, const std::filesystem::path& directory);
bool Enabled();    // Tracking (CreateVS, swaps).
bool Streaming();  // Markers this frame.
void OnSwap();     // After the guest Swap: the next frame.
void OnCreateVS(uint32_t source, uint32_t object);
void AfterDraw(uint32_t device);
void Composition(bool after);
void BeforeResolve(uint32_t device, uint32_t caller, uint32_t flags);  // D3D Resolve entry.
void BeforeSwap(uint32_t device);
void NoteFrameTime(float seconds);       // Simulated frame time (physics step input).
void NoteGameplayCamera(bool gameplay);  // Player camera vs cutscene/scripted control, per Swap.
void Shutdown();
}  // namespace pdz::engine::scene
