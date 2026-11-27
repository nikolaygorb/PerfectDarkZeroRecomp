#include "scene_probe.h"
#include "device_layout.h"
#include "../../mod/third_person.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/graphics/title_scene.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>

REXCVAR_DEFINE_BOOL(pdz_engine_scene_stream, false, "Render/Engine",
    "Emit scene markers every frame for the Native scene consumer. On by itself while the SDK's "
    "consumer is (native_scene_frames, native_temporal_upscaler, native_frame_generation); this forces it "
    "for diagnostics.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_engine_scene_probe, false, "Render/Engine",
    "Capture series only: an SDK scene probe (target export) before every scene-sized resolve, "
    "to find where the shaded scene is complete in a new build. Needs pdz_engine_report_dir.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(pdz_engine_capture_request, 0, "Render/Engine",
    "Scene capture: change to a new nonzero number (F4) to capture a series after "
    "pdz_engine_capture_delay_frames; the SDK exports its frames into pdz_engine_report_dir.")
    .range(0, INT32_MAX).lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_engine_capture_frames, 1, "Render/Engine",
    "Scene capture: consecutive frames per request.")
    .range(1, 4).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(pdz_engine_capture_delay_frames, 0, "Render/Engine",
    "Scene capture: frames to wait after a request (time to close F4 and move the camera).")
    .range(0, 600).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pdz_engine_scene_auto_opaque, true, "Render/Engine",
    "Scene stream: ask the SDK to snapshot opaque color before the first blended scene draw (reactive mask).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_engine_scene_locked_distance, 80.0, "Render/Engine",
    "Scene stream: pixels nearer than this (world units, ~cm) are the first-person weapon and keep zero "
    "motion (80 checked in game 2026-11-24). 0 off; native_scene_debug=4 shows it green.")
    .range(0.0, 500.0).lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(pdz_engine_scene_end_caller, "82612234", "Render/Engine",
    "Scene stream: return address (hex) of the D3D Resolve that ends the scene; scene end (and the temporal "
    "upscaler's output) goes right before it. 82612234 or 826121CC: resolves of the shaded scene, before "
    "post-processing (both checked in game). 0: composition entry only.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_engine_scene_cut_distance, 500.0, "Render/Engine",
    "Scene stream: a camera position jump above this (world units per frame) resets temporal history.")
    .range(10.0, 100000.0).lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(pdz_engine_scene_frame_rect, "0 0 1280 720", "Render/Engine",
    "Scene stream: where the 1216x640 scene appears in the 1280x720 frame, guest pixels \"x y width height\" "
    "(frame generation maps depth and motion there). Check with native_frame_generation_debug=4: the colored "
    "inputs must lie exactly over the 3D picture. Empty: unknown (SDK assumes the origin, 1:1).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(pdz_engine_scene_player_volume, "45 175 25", "Render/Engine",
    "Scene stream, third person: the player character moves with the camera; a box around it (world units "
    "\"radius below-eyes above-eyes\") is marked camera-locked (zero motion) for FSR and frame generation. "
    "Check with native_scene_debug=4: the character green. Empty: off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_engine_scene_generate_cutscenes, true, "Render/Engine",
    "Scene stream: allow frame generation in cutscenes and scripted cameras (capped at 30 FPS by the "
    "cutscene pacing). Off: real frames only there.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace pdz::engine::scene {
namespace {
using rex::graphics::title::ScenePoint;
// PDZ launch facts (checked in game, November 2026).
constexpr uint64_t kViewSource = 0x94D1BC182C9B14A9ull;  // World VS source (CreateVS bytes, XXH3).
// Other world VS variants (levels use their own), verified by the SDK in game
// (found by the learning below, campaign pass 2026-11-25).
constexpr uint64_t kViewSourceVariants[] = {0x93924E5370693483ull, 0x323C2235409C13EBull,
                                            0xA874C6E8B5F2E1FCull};
constexpr uint32_t kCameraRegister = 7;                  // c7-c10 = projection * view.
constexpr uint32_t kSceneWidth = 1216, kSceneHeight = 640;
constexpr float kMetersPerUnit = .01f;                   // World units are centimetres.
constexpr uint32_t kDeviceGlobal = 0x82E2A7B0u;
constexpr uint32_t kVertexShaderSlot = 20456;            // Device: current VS object.
// Scene end goes before a resolve of the shaded scene (pdz_engine_scene_end_caller),
// so a temporal upscaler's output reaches the post chain and the composition.
enum ResetReason : uint32_t { kResetNone, kResetCameraControl, kResetTeleport };

// enabled: the SDK takes markers (shader identities are tracked from the
// start); producing: markers this frame; stream_changed: re-read at the swap.
std::atomic<bool> enabled{false}, producing{false}, view_needed{false}, capture_requested{false};
std::atomic<bool> stream_changed{false};
std::atomic<uint32_t> frame_time_bits{0};
std::atomic<bool> frame_time_fresh{false};
std::mutex mutex;
rex::memory::Memory* memory = nullptr;
rex::system::IGraphicsSystem* graphics = nullptr;
std::filesystem::path report_path;
uint64_t frame = 1, first = 0, end = 0, session = 0, marked_view = 0;
uint64_t queued = 0, ring_skips = 0, invalid = 0, views = 0, resets[3]{};
uint64_t skips_by_point[8]{};
uint64_t scene_end_at_resolve = 0, scene_end_fallback = 0, scene_end_missed = 0, scene_end_frame = 0;  // Ring-space skips per ScenePoint: each lost marker breaks history.
bool stream = false;
std::unordered_set<uint32_t> view_shaders;  // CreateVS objects with a world source.
// World VS sources: the known one, plus variants learned this session (levels
// use other variants). Learning: after a second without views, a shader drawn
// at the scene viewport is offered as kView once; the SDK verifies it reads
// the camera registers and they hold the camera (accepted views and their
// source in its stats).
std::unordered_set<uint64_t> view_sources{kViewSource};
// Variants learned in earlier sessions, one hex hash per line (next to the game).
std::filesystem::path LearnedSourcesPath() {
  return rex::filesystem::GetExecutableFolder() / "cache" / "pdz_scene_shaders.txt";
}
void LoadLearnedSources() {
  for (const uint64_t source : kViewSourceVariants) view_sources.insert(source);
  std::ifstream file(LearnedSourcesPath());
  std::string line;
  while (std::getline(file, line)) {
    const uint64_t source = std::strtoull(line.c_str(), nullptr, 16);
    if (source) view_sources.insert(source);
  }
}
void SaveLearnedSource(uint64_t source) {
  std::error_code error;
  std::filesystem::create_directories(LearnedSourcesPath().parent_path(), error);
  std::ofstream file(LearnedSourcesPath(), std::ios::app);
  char text[24];
  std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(source));
  file << text << '\n';
  if (!file) REXLOG_WARN("PDZ scene: cannot save the learned shader to {}", LearnedSourcesPath().string());
}
std::unordered_map<uint64_t, uint32_t> candidate_attempts;
struct Attempt { uint64_t source = 0, frame = 0, accepted_before = 0; } attempt;
// The last queued view (world -> view 3x4) and third-person character volumes sent.
std::array<float, 12> last_view{};
uint64_t locked_volumes = 0;
// Scenes at another viewport than gameplay's 1216x640 (cutscenes, menus with
// 3D): after a second without a view, the world shader at any large viewport
// is the view; the largest such pass of a frame wins (a later, larger one
// re-sends kView). Ends at the next gameplay-sized view.
constexpr uint32_t kAdaptiveMinArea = 640 * 360;
bool adaptive_scene = false;
uint32_t marked_view_area = 0;
uint64_t adaptive_views = 0, adaptive_entries = 0;
std::map<uint32_t, uint64_t> refused_viewports;  // width << 16 | height -> count (diagnostics).
// Views refused for the draw state: predicated tiling, a viewport off the
// target origin (samples "x y w h tiling" -> count).
uint64_t view_refused_tiling = 0, view_refused_origin = 0;
std::map<std::string, uint64_t> refused_states;
// Diagnostics of frames without a view: every CreateVS object's source hash,
// the hashes of untracked shaders drawn at the scene viewport, kView refusals.
std::unordered_map<uint32_t, uint64_t> shader_sources;
std::unordered_set<uint64_t> untracked_scene_sources;
uint64_t untracked_scene_draws = 0, view_refused_viewport = 0, view_refused_projection = 0;
uint64_t frames_without_view = 0, untracked_checks_frame = 0, view_streak = 0;
uint32_t untracked_checks = 0;
bool gameplay_camera = true, gameplay_known = false, pending_control_reset = false;
std::array<double, 3> previous_eye{};
uint64_t previous_eye_frame = 0;

uint8_t* Access(uint32_t address, uint32_t bytes, bool write = false) {
  if (!memory || !address || !bytes || uint64_t(address) + bytes > 0x100000000ull) return nullptr;
  const auto access = memory->QueryRangeAccess(address, bytes);
  if (access == rex::memory::PageAccess::kNoAccess || (write && !(uint32_t(access) & 2))) return nullptr;
  return memory->TranslateVirtual<uint8_t*>(address);
}
bool Active() { return enabled.load(std::memory_order_relaxed) && first && frame >= first && frame < end; }
bool Producing() { return stream || Active(); }
// The scene in the frame (pdz_engine_scene_frame_rect); false: unknown.
bool FrameRect(std::array<uint16_t, 4>& rect) {
  const auto& text = REXCVAR_GET(pdz_engine_scene_frame_rect);
  const char* cursor = text.c_str();
  for (size_t i = 0; i < 4; ++i) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(cursor, &end, 10);
    if (end == cursor || value > 4096) return false;
    rect[i] = uint16_t(value);
    cursor = end;
  }
  return rect[2] && rect[3];
}
uint32_t SceneEndCaller() {
  return uint32_t(std::strtoul(REXCVAR_GET(pdz_engine_scene_end_caller).c_str(), nullptr, 16));
}
void Report() {
  if (report_path.empty()) return;
  uint64_t stats[3]{};
  if (graphics) graphics->QueryTitleSceneStats(stats, 3);
  std::ofstream file(report_path);
  file << "{\"schema\":3,\"session\":" << session << ",\"frame\":" << frame
       << ",\"queued\":" << queued << ",\"views\":" << views << ",\"ring_skips\":" << ring_skips
       << ",\"skips_view\":" << skips_by_point[1] << ",\"skips_scene_end\":" << skips_by_point[2]
       << ",\"skips_post_end\":" << skips_by_point[3] << ",\"skips_frame_end\":" << skips_by_point[4]
       << ",\"scene_end_at_resolve\":" << scene_end_at_resolve << ",\"scene_end_fallback\":" << scene_end_fallback
       << ",\"scene_end_missed\":" << scene_end_missed
       << ",\"frames_without_view\":" << frames_without_view << ",\"view_refused_viewport\":" << view_refused_viewport
       << ",\"view_refused_projection\":" << view_refused_projection << ",\"untracked_scene_draws\":" << untracked_scene_draws
       << ",\"view_sources\":[";
  {
    bool first_source = true;
    for (const uint64_t source : view_sources) {
      char text[24];
      std::snprintf(text, sizeof(text), "\"%016llX\"", static_cast<unsigned long long>(source));
      file << (first_source ? "" : ",") << text;
      first_source = false;
    }
  }
  file << "],\"untracked_scene_sources\":[";
  {
    bool first_source = true;
    for (const uint64_t source : untracked_scene_sources) {
      char text[24];
      std::snprintf(text, sizeof(text), "\"%016llX\"", static_cast<unsigned long long>(source));
      file << (first_source ? "" : ",") << text;
      first_source = false;
    }
  }
  file << "]"
       << ",\"locked_volumes\":" << locked_volumes << ",\"adaptive_entries\":" << adaptive_entries
       << ",\"adaptive_views\":" << adaptive_views << ",\"view_refused_tiling\":" << view_refused_tiling
       << ",\"view_refused_origin\":" << view_refused_origin << ",\"refused_states\":{";
  {
    bool first_state = true;
    for (const auto& [key, count] : refused_states) {
      file << (first_state ? "" : ",") << "\"" << key << "\":" << count;
      first_state = false;
    }
  }
  file << "},\"refused_viewports\":{";
  {
    bool first_size = true;
    for (const auto& [size, count] : refused_viewports) {
      file << (first_size ? "" : ",") << "\"" << (size >> 16) << 'x' << (size & 0xFFFF) << "\":" << count;
      first_size = false;
    }
  }
  file << "}"
       << ",\"invalid\":" << invalid << ",\"resets_camera_control\":" << resets[kResetCameraControl]
       << ",\"resets_teleport\":" << resets[kResetTeleport] << ",\"view_shaders\":" << view_shaders.size()
       << ",\"sdk_consumed\":" << stats[0] << ",\"sdk_completed\":" << stats[1] << ",\"sdk_failed\":" << stats[2] << "}\n";
  file.flush();
  if (!file) REXLOG_ERROR("PDZ scene: cannot persist producer report {}", report_path.string());
}
// Eye position from the 3x4 view rows: eye = -R^T t.
std::array<double, 3> Eye(const std::array<float, 12>& v) {
  std::array<double, 3> eye{};
  for (size_t x = 0; x < 3; ++x)
    for (size_t y = 0; y < 3; ++y) eye[x] -= double(v[y * 4 + x]) * v[y * 4 + 3];
  return eye;
}
// Evaluated per attempt; committed only once the marker is in the stream.
uint32_t ResetFor(const std::array<double, 3>& eye) {
  if (pending_control_reset) return kResetCameraControl;
  if (previous_eye_frame && previous_eye_frame + 1 == frame) {
    const double dx = eye[0] - previous_eye[0], dy = eye[1] - previous_eye[1], dz = eye[2] - previous_eye[2];
    const double limit = REXCVAR_GET(pdz_engine_scene_cut_distance);
    if (dx * dx + dy * dy + dz * dz > limit * limit) return kResetTeleport;
  }
  return kResetNone;
}
// Third person: a view-space box around the player character, which the camera
// follows (it keeps its place on screen: camera-locked). From the player's
// camera mode (third_person.h) while its camera is away from the eyes and
// the view queued this frame is that camera. World up: the world axis the
// view's up row points along most (the camera doesn't roll).
bool PlayerVolume(std::array<float, 6>& box) {
  const uint32_t mode = third_person::PlayerCameraMode();
  const auto& text = REXCVAR_GET(pdz_engine_scene_player_volume);
  if (!mode || text.empty()) return false;
  double size[3]{};
  const char* cursor = text.c_str();
  for (double& value : size) {
    char* end = nullptr;
    value = std::strtod(cursor, &end);
    if (end == cursor || !(value >= 0) || value > 1000) return false;
    cursor = end;
  }
  const auto* camera_bytes = Access(mode + third_person::kModeCameraPosition, 12);
  const auto* eye_bytes = Access(mode + third_person::kModeEyePosition, 12);
  if (!camera_bytes || !eye_bytes) return false;
  std::array<double, 3> camera{}, eye{};
  double away = 0;
  for (size_t i = 0; i < 3; ++i) {
    camera[i] = std::bit_cast<float>(Be32(camera_bytes + i * 4));
    eye[i] = std::bit_cast<float>(Be32(eye_bytes + i * 4));
    if (!std::isfinite(camera[i]) || !std::isfinite(eye[i])) return false;
    away += (camera[i] - eye[i]) * (camera[i] - eye[i]);
  }
  if (away < 30. * 30.) return false;  // First person (the weapon has the locked distance).
  // The view must be this camera's (not a cutscene's or another player's).
  const auto view_camera = Eye(last_view);
  double mismatch = 0;
  for (size_t i = 0; i < 3; ++i) mismatch += (view_camera[i] - camera[i]) * (view_camera[i] - camera[i]);
  if (mismatch > 50. * 50.) return false;
  size_t up = 0;
  for (size_t i = 1; i < 3; ++i)
    if (std::abs(last_view[4 + i]) > std::abs(last_view[4 + up])) up = i;
  const double up_sign = last_view[4 + up] >= 0 ? 1. : -1.;
  const size_t h1 = (up + 1) % 3, h2 = (up + 2) % 3;
  std::array<double, 6> bounds = {INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY};
  for (int corner = 0; corner < 8; ++corner) {
    std::array<double, 3> p = eye;
    p[h1] += corner & 1 ? size[0] : -size[0];
    p[h2] += corner & 2 ? size[0] : -size[0];
    p[up] += up_sign * (corner & 4 ? size[2] : -size[1]);
    for (size_t row = 0; row < 3; ++row) {
      const double v = last_view[row * 4] * p[0] + last_view[row * 4 + 1] * p[1] +
                       last_view[row * 4 + 2] * p[2] + last_view[row * 4 + 3];
      bounds[row] = std::min(bounds[row], v);
      bounds[3 + row] = std::max(bounds[3 + row], v);
    }
  }
  for (size_t i = 0; i < 6; ++i) {
    if (!std::isfinite(bounds[i])) return false;
    box[i] = float(bounds[i]);
  }
  return true;
}
bool Queue(uint32_t device_address, ScenePoint point, uint64_t source_hash,
           const std::array<float, 6>* locked_box = nullptr) {
  using namespace rex::graphics::title;
  auto* device = Access(device_address, kDeviceBytes, true);
  DeviceSnapshot state;
  if (!device || !SnapshotDevice({device, kDeviceBytes}, state)) { ++invalid; return false; }
  // Gameplay draws the scene at the target origin; other viewports are taken
  // while adaptive. A cutscene letterbox (full width between equal bars) at
  // once: its bars slide in during the first second, before adaptive.
  const bool off_origin = state.viewport[0] || state.viewport[1];
  const uint32_t bars = 2 * state.viewport[1] + state.viewport[3];
  const bool letterbox = !state.viewport[0] && state.viewport[1] && state.viewport[2] == kSceneWidth &&
                         state.viewport[3] >= kSceneHeight / 2 && bars <= kSceneHeight && bars + 3 >= kSceneHeight;
  if (state.tiling || (off_origin && !adaptive_scene && !letterbox) || !state.viewport[2] || !state.viewport[3]) {
    ++invalid;
    if (point == ScenePoint::kView) {
      ++(state.tiling ? view_refused_tiling : view_refused_origin);
      const std::string key = std::to_string(state.viewport[0]) + ' ' + std::to_string(state.viewport[1]) + ' ' +
                              std::to_string(state.viewport[2]) + ' ' + std::to_string(state.viewport[3]) + ' ' +
                              std::to_string(state.tiling);
      if (refused_states.size() < 24 || refused_states.contains(key)) ++refused_states[key];
    }
    return false;
  }
  SceneMarker marker;
  std::array<double, 3> eye{};
  marker.point = point; marker.frame = frame; marker.session = session; marker.device = device_address;
  marker.width = state.viewport[2]; marker.height = state.viewport[3]; marker.source_hash = source_hash;
  if (Active()) marker.flags |= kSceneCapture;
  // Frame generation: where the composition puts this view's viewport (the
  // configured rectangle is the 1216x640 surface's; a letterboxed viewport is
  // its part). On kView only: other points have other viewports.
  if (std::array<uint16_t, 4> rect{}; point == ScenePoint::kView && FrameRect(rect)) {
    const double sx = double(rect[2]) / kSceneWidth, sy = double(rect[3]) / kSceneHeight;
    marker.frame_x = uint16_t(std::lround(rect[0] + state.viewport[0] * sx));
    marker.frame_y = uint16_t(std::lround(rect[1] + state.viewport[1] * sy));
    marker.frame_width = uint16_t(std::max(1L, std::lround(state.viewport[2] * sx)));
    marker.frame_height = uint16_t(std::max(1L, std::lround(state.viewport[3] * sy)));
  }
  if (gameplay_known && !gameplay_camera && !REXCVAR_GET(pdz_engine_scene_generate_cutscenes))
    marker.flags |= kSceneNoFrameGeneration;
  if (point == ScenePoint::kLockedVolume) {
    if (!locked_box) return false;
    marker.locked_box = *locked_box;
  }
  if (point == ScenePoint::kView) {
    const auto* constants = Access(device_address + kConstantBegin, 44 * 4);
    if (!constants) { ++invalid; return false; }
    std::array<float, 44> words{};
    for (size_t i = 0; i < words.size(); ++i) {
      words[i] = std::bit_cast<float>(Be32(constants + i * 4));
      if (!std::isfinite(words[i])) { ++invalid; return false; }
    }
    // The world pass: scene viewport and a right-handed perspective (w = -z).
    const bool gameplay_size = marker.width == kSceneWidth && marker.height == kSceneHeight;
    if (!gameplay_size && !letterbox && !(adaptive_scene && marker.width * marker.height >= kAdaptiveMinArea)) {
      ++view_refused_viewport;
      if (refused_viewports.size() < 32 || refused_viewports.contains(marker.width << 16 | marker.height))
        ++refused_viewports[marker.width << 16 | marker.height];
      return false;
    }
    if (std::abs(words[26] + 1.f) > 1e-5f) { ++view_refused_projection; return false; }
    std::copy_n(words.begin(), 12, marker.camera.view.begin());
    std::copy_n(words.begin() + 12, 16, marker.camera.projection.begin());
    std::copy_n(words.begin() + 28, 16, marker.camera.view_projection.begin());
    marker.flags |= kSceneCamera;
    marker.camera_register = kCameraRegister;
    if (frame_time_fresh.exchange(false, std::memory_order_acq_rel))
      marker.delta_time = std::bit_cast<float>(frame_time_bits.load(std::memory_order_relaxed));
    if (!std::isfinite(marker.delta_time) || marker.delta_time < 0 || marker.delta_time > 1) marker.delta_time = 0;
    marker.locked_distance = float(REXCVAR_GET(pdz_engine_scene_locked_distance));
    marker.meters_per_unit = kMetersPerUnit;
    if (REXCVAR_GET(pdz_engine_scene_auto_opaque)) marker.flags |= kSceneAutoOpaque;
    eye = Eye(marker.camera.view);
    marker.reset_reason = ResetFor(eye);
    if (marker.reset_reason) marker.flags |= kSceneReset;
  }
  const uint32_t cursor = Be32(device), safe_end = Be32(device + 8);
  constexpr uint32_t bytes = (uint32_t(kSceneWords) + 1) * 4;
  // Append only inside already reserved space. Never relocate the segment,
  // call a reserve helper, overwrite pending UP data, or run a draw twice.
  if ((cursor & 3) || uint64_t(cursor) + bytes > safe_end || uint64_t(cursor) + bytes + 4 > 0x100000000ull) {
    ++ring_skips; ++skips_by_point[uint32_t(point) & 7]; return false;
  }
  auto* output = Access(cursor + 4, bytes, true);
  if (!output) { ++invalid; return false; }
  const auto words = EncodeScene(marker);
  PutBe32(output, SceneHeader());
  for (size_t i = 0; i < words.size(); ++i) PutBe32(output + 4 + i * 4, words[i]);
  PutBe32(device, cursor + bytes);
  ++queued;
  if (point == ScenePoint::kView) {
    last_view = marker.camera.view;
    marked_view_area = marker.width * marker.height;
    if (marker.width == kSceneWidth && marker.height == kSceneHeight) {
      if (adaptive_scene) REXLOG_INFO("PDZ scene: gameplay view again, adaptive scene detection off");
      adaptive_scene = false;
    } else {
      ++adaptive_views;
    }
    ++views;
    if (marker.reset_reason) ++resets[marker.reset_reason];
    pending_control_reset = false; previous_eye = eye; previous_eye_frame = frame;
  }
  return true;
}
bool StreamWanted() {
  if (REXCVAR_GET(pdz_engine_scene_stream)) return true;
  // The GPU plugin's flags, by name (they live in its module).
  const auto frames = rex::cvar::GetFlagByName("native_scene_frames");
  const auto upscaler = rex::cvar::GetFlagByName("native_temporal_upscaler");
  const auto generation = rex::cvar::GetFlagByName("native_frame_generation");  // The presenter's.
  return frames == "true" || frames == "1" || (!upscaler.empty() && upscaler != "off") ||
         (!generation.empty() && generation != "off");
}
}  // namespace

void Initialize(rex::Runtime& runtime, const std::filesystem::path& directory) {
  std::lock_guard lock(mutex);
  graphics = runtime.graphics_system();
  // The backend takes this protocol or a newer one.
  if (!graphics || graphics->GetTitleSceneProtocolVersion() < rex::graphics::title::kSceneProtocolVersion) {
    // The emulated plugin, for one: only worth a warning when asked for.
    if (StreamWanted())
      REXLOG_WARN("PDZ scene: GPU backend does not support scene protocol {}", rex::graphics::title::kSceneProtocolVersion);
    return;
  }
  memory = runtime.memory();
  LoadLearnedSources();
  stream = StreamWanted();
  session = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  if (!directory.empty()) {
    report_path = directory / "scene-producer.json";
    if (!rex::cvar::SetFlagByName("native_scene_probe_dir", (directory / "native").generic_string()) ||
        !rex::cvar::SetFlagByName("native_scene_probe_tracking", "true")) {
      REXLOG_ERROR("PDZ scene: output directory could not be configured"); return;
    }
  }
  rex::cvar::RegisterChangeCallback("pdz_engine_capture_request", [](std::string_view, std::string_view value) {
    if (value != "0") capture_requested.store(true, std::memory_order_release);
  });
  if (REXCVAR_GET(pdz_engine_capture_request)) capture_requested.store(true, std::memory_order_release);
  // The stream follows the consumer while the game runs (F4), from the next frame.
  for (const char* name : {"native_temporal_upscaler", "native_scene_frames", "pdz_engine_scene_stream"})
    rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) {
      stream_changed.store(true, std::memory_order_release);
    });
  enabled.store(true, std::memory_order_release);
  producing.store(Producing(), std::memory_order_release);
  view_needed.store(Producing(), std::memory_order_release);
  Report();
  REXLOG_INFO("PDZ scene: protocol {} enabled, stream {}, session {}, reports {}",
              rex::graphics::title::kSceneProtocolVersion, stream, session,
              directory.empty() ? std::string("off") : directory.string());
}
bool Enabled() { return enabled.load(std::memory_order_relaxed); }
bool Streaming() { return producing.load(std::memory_order_relaxed); }
void OnSwap() {
  if (!enabled.load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex);
  if (Producing() && marked_view != frame) { ++frames_without_view; ++view_streak; } else view_streak = 0;
  if (view_streak >= 60 && !adaptive_scene) {
    adaptive_scene = true;
    ++adaptive_entries;
    REXLOG_INFO("PDZ scene: no gameplay view for a second, adaptive scene detection on (cutscenes, other viewports)");
  }
  // A candidate's verdict, once the GPU stream has passed its frame.
  if (attempt.source && frame >= attempt.frame + 8) {
    uint64_t stats[5]{};
    if (graphics && graphics->QueryTitleSceneStats(stats, 5) && stats[3] > attempt.accepted_before &&
        stats[4] == attempt.source && view_sources.insert(attempt.source).second) {
      for (const auto& [object, hash] : shader_sources)
        if (hash == attempt.source) view_shaders.insert(object);
      REXLOG_INFO("PDZ scene: world VS variant {:016X} learned (the SDK verified its camera)", attempt.source);
      SaveLearnedSource(attempt.source);
    }
    attempt = {};
  }
  ++frame;
  if (stream_changed.exchange(false, std::memory_order_acq_rel) && stream != StreamWanted()) {
    // A new stream starts with a history reset (the SDK sees a frame gap).
    stream = !stream;
    REXLOG_INFO("PDZ scene: stream {}", stream ? "on" : "off");
  }
  const bool finished = first && frame == end;
  // A capture series (kSceneCapture frames) starts after the current one ends.
  if (frame >= end && capture_requested.exchange(false, std::memory_order_acq_rel)) {
    first = frame + uint64_t(REXCVAR_GET(pdz_engine_capture_delay_frames));
    end = first + uint64_t(REXCVAR_GET(pdz_engine_capture_frames));
    REXLOG_INFO("PDZ scene: capture of {} frames from {}", end - first, first);
  }
  producing.store(Producing(), std::memory_order_release);
  view_needed.store(Producing(), std::memory_order_release);
  if (finished || frame % 600 == 0) Report();
}
void OnCreateVS(uint32_t source, uint32_t object) {
  if (!enabled.load(std::memory_order_acquire) || !object) return;
  // Shader container header, then XXH3 of the code.
  uint64_t hash = 0;
  {
    std::lock_guard lock(mutex);
    if (const auto* header = Access(source, 12)) {
      const uint32_t magic = Be32(header), header_bytes = Be32(header + 4), bytes = Be32(header + 8);
      if (magic == 0x102A0E01 && header_bytes >= 12 && header_bytes <= 65536 && bytes && bytes <= 1024 * 1024 &&
          uint64_t(source) + header_bytes + bytes <= 0x100000000ull)
        if (const auto* code = Access(source + header_bytes, bytes)) hash = XXH3_64bits(code, bytes);
    }
    // A reused object address loses its previous identity.
    if (view_sources.contains(hash)) view_shaders.insert(object); else view_shaders.erase(object);
    shader_sources[object] = hash;
  }
}
void AfterDraw(uint32_t device) {
  if (!enabled.load(std::memory_order_acquire) || !view_needed.load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex);
  const auto* data = Access(device + kVertexShaderSlot, 4);
  if (!data) return;
  if (!view_shaders.contains(Be32(data))) {
    // Diagnostics: after a second without views, the shaders drawn at the
    // scene viewport (a few draws a frame).
    if (Producing() && marked_view != frame && view_streak >= 60) {
      if (untracked_checks_frame != frame) { untracked_checks_frame = frame; untracked_checks = 0; }
      DeviceSnapshot state;
      const auto* bytes = Access(device, kDeviceBytes);
      if (untracked_checks < 32 && bytes && SnapshotDevice({bytes, kDeviceBytes}, state) &&
          state.viewport[2] == kSceneWidth && state.viewport[3] == kSceneHeight) {
        ++untracked_checks; ++untracked_scene_draws;
        const auto found = shader_sources.find(Be32(data));
        const uint64_t source = found != shader_sources.end() ? found->second : 0;
        if (untracked_scene_sources.size() < 24) untracked_scene_sources.insert(source);
        // One candidate in flight, two tries per source.
        uint64_t stats[5]{};
        if (source && !attempt.source && candidate_attempts[source] < 2 && graphics &&
            graphics->QueryTitleSceneStats(stats, 5) && Queue(device, ScenePoint::kView, source)) {
          marked_view = frame;
          view_needed.store(false, std::memory_order_release);
          attempt = {source, frame, stats[3]};
          ++candidate_attempts[source];
        }
      }
    }
    return;
  }
  const auto known = shader_sources.find(Be32(data));
  const uint64_t source = known != shader_sources.end() ? known->second : kViewSource;
  if (!Producing()) return;
  if (marked_view == frame) {
    // Adaptive: only a larger pass of the world shader replaces this frame's view.
    if (!adaptive_scene) return;
    DeviceSnapshot state;
    const auto* bytes = Access(device, kDeviceBytes);
    if (!bytes || !SnapshotDevice({bytes, kDeviceBytes}, state) ||
        state.viewport[2] * state.viewport[3] <= marked_view_area) return;
  }
  if (Queue(device, ScenePoint::kView, source)) {
    marked_view = frame;
    // Adaptive scenes keep watching for a larger pass this frame.
    view_needed.store(adaptive_scene, std::memory_order_release);
    if (std::array<float, 6> box{}; PlayerVolume(box) && Queue(device, ScenePoint::kLockedVolume, 0, &box))
      ++locked_volumes;
  }
}
void BeforeSwap(uint32_t device) {
  if (!enabled.load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex);
  if (Producing()) Queue(device, ScenePoint::kFrameEnd, 0);
}
void BeforeResolve(uint32_t device, uint32_t caller, uint32_t flags) {
  if (!enabled.load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex);
  // Capture series: a target snapshot before every resolve (caller in the
  // probe's source_hash) to find where the shaded scene is complete.
  if (Active() && REXCVAR_GET(pdz_engine_scene_probe)) {
    DeviceSnapshot state;
    const auto* data = Access(device, kDeviceBytes);
    if (data && SnapshotDevice({data, kDeviceBytes}, state) && state.viewport[2] == kSceneWidth &&
        state.viewport[3] == kSceneHeight) Queue(device, ScenePoint::kProbe, caller);
  }
  const uint32_t scene_end = SceneEndCaller();
  if (!scene_end || caller != scene_end || flags != 0 || !Producing() || scene_end_frame == frame) return;
  if (Queue(device, ScenePoint::kSceneEnd, 0)) { scene_end_frame = frame; ++scene_end_at_resolve; }
}
void Composition(bool after) {
  if (!enabled.load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex);
  if (!Producing()) return;
  const auto* global = Access(kDeviceGlobal, 4);
  if (!global) { ++invalid; return; }
  // Scene end at the composition only without a configured resolve caller.
  // With one, its marker was lost (ring space): the target bound here is not
  // the scene's, and ending the scene on it would switch the upscaler's
  // source for one frame (context re-creation, two history resets). The
  // missed frame costs one reset instead.
  if (!after && scene_end_frame != frame) {
    // Adaptive scenes may resolve elsewhere: scene end at the composition.
    if (SceneEndCaller() && !adaptive_scene) ++scene_end_missed;
    else if (Queue(Be32(global), ScenePoint::kSceneEnd, 0)) { scene_end_frame = frame; ++scene_end_fallback; }
    return;
  }
  // After the composition only the HUD follows.
  if (after) Queue(Be32(global), ScenePoint::kPostEnd, 0);
}
void NoteFrameTime(float seconds) {
  frame_time_bits.store(std::bit_cast<uint32_t>(seconds), std::memory_order_relaxed);
  frame_time_fresh.store(true, std::memory_order_release);
}
void NoteGameplayCamera(bool gameplay) {
  if (!enabled.load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex);
  if (gameplay_known && gameplay != gameplay_camera) pending_control_reset = true;
  gameplay_camera = gameplay; gameplay_known = true;
}
void Shutdown() {
  if (!enabled.exchange(false, std::memory_order_acq_rel)) return;
  std::lock_guard lock(mutex);
  Report(); memory = nullptr; graphics = nullptr; view_shaders.clear();
}
}  // namespace pdz::engine::scene
