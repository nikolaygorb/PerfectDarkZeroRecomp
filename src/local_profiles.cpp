#include "generated/default/perfectdarkzerorecomp_init.h"
#include "local_profiles.h"
#include "profile_store.h"
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <imgui.h>
#include <atomic>
#include <cstring>
#include <fstream>
#ifdef _WIN32


#include <windows.h>
#endif

namespace pdz_profiles {
namespace {
Store store;
std::filesystem::path file;
uint32_t running_id = 0;
std::string running_name = "User", load_error;
std::atomic<bool> open{false};
std::atomic<bool> notify_on_close{false};

void Save(const Store& updated) {
  if (!load_error.empty()) throw std::runtime_error(load_error);
  Validate(updated);
  std::filesystem::create_directories(file.parent_path());
  auto temporary = file; temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    WriteStore(out, updated); out.flush();
    if (!out) throw std::runtime_error("Could not save profiles. Check the save folder is writable.");
  }
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Could not replace the profile file. Your previous profiles are unchanged.");
#else
  std::filesystem::rename(temporary, file);
#endif
  store = updated;
}

class Dialog final : public rex::ui::ImGuiDialog {
 public:
  explicit Dialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
 private:
  char name_[16]{};
  std::string error_;
  bool was_open_ = false;
  void OnDraw(ImGuiIO&) override {
    if (!open) {
      if (was_open_ && notify_on_close) {
        auto* ks = rex::Runtime::instance()->kernel_state();
        ks->BroadcastNotification(0xA, 1);
        ks->BroadcastNotification(0x9, 0);
        notify_on_close = false;
      }
      was_open_ = false; return;
    }
    was_open_ = true;
    bool visible = true;
    ImGui::SetNextWindowSize(ImVec2(610, 410), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Local profiles (F9 to close)", &visible)) {
      ImGui::SetWindowFontScale(1.35f);
      ImGui::Text("Playing as: %s", running_name.c_str());
      ImGui::TextWrapped("Select a profile for the next launch. New profiles have separate saves. The original profile keeps your existing progress.");
      ImGui::Separator();
      // Selecting persists a replacement Store; iterate a stable snapshot.
      const auto profile_list = store.profiles;
      for (const auto& p : profile_list) {
        ImGui::PushID(int(p.id));
        if (ImGui::Selectable(p.name.c_str(), store.selected == p.id)) {
          try { auto updated = store; updated.selected = p.id; Save(updated); error_.clear(); }
          catch (const std::exception& e) { error_ = e.what(); }
        }
        ImGui::PopID();
      }
      if (store.selected != running_id)
        ImGui::TextWrapped("Profile saved. Close and relaunch PDZ to switch profiles.");
      ImGui::Separator();
      ImGui::InputText("Profile name", name_, sizeof(name_));
      if (ImGui::Button("Create profile")) {
        try { auto updated = store; AddProfile(updated, name_); Save(updated); name_[0] = 0; error_.clear(); }
        catch (const std::exception& e) { error_ = e.what(); }
      }
      ImGui::SameLine();
      if (ImGui::Button("Rename selected")) {
        try {
          auto updated = store;
          for (auto& p : updated.profiles) if (p.id == updated.selected) p.name = name_;
          Save(updated);
          // The guest caches its name when signing in; apply renames at restart too.
          error_ = "Name saved. Close and relaunch PDZ to refresh its menus.";
          name_[0] = 0;
        } catch (const std::exception& e) { error_ = e.what(); }
      }
      ImGui::TextWrapped("Names: 1-15 letters, numbers, spaces, - or _. One active local player profile per launch.");
      if (!load_error.empty()) ImGui::TextWrapped("%s", load_error.c_str());
      if (!error_.empty()) ImGui::TextWrapped("%s", error_.c_str());
      if (ImGui::Button("Close")) visible = false;
    }
    ImGui::End();
    if (!visible) open = false;
  }
};
}

void Configure(std::filesystem::path& root) {
  store = Store{};
  load_error.clear();
  running_name = "User";
  file = root / "local_profiles.txt";
  try {
    if (std::filesystem::exists(file)) {
      std::ifstream in(file, std::ios::binary);
      if (!in) throw std::runtime_error("Could not read profiles. Existing saves have not been changed.");
      store = ReadStore(in);
    }
  } catch (const std::exception& e) { load_error = e.what(); }
  running_id = store.selected;
  for (const auto& p : store.profiles) if (p.id == running_id) running_name = p.name;
  // Never move existing saves, or use a display name as a filesystem component.
  // Keep the SDK identity stable; the root isolates every additional profile.
  if (running_id) root /= std::filesystem::path("profiles") / std::to_string(running_id);
}
std::unique_ptr<rex::ui::ImGuiDialog> MakeProfileDialog(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<Dialog>(drawer);
}
void Toggle() { open = !open.load(); }
bool IsOpen() { return open.load(); }
void RequestSignin() {
  // The current profile remains signed in. No online authentication is implied.
  notify_on_close = true;
  rex::Runtime::instance()->kernel_state()->BroadcastNotification(0x9, 1);
  open = true;
}
std::string Name() { return running_name; } // Immutable after Configure, before guest launch.
}

REX_EXTERN(__imp__sub_82ACA290);
REX_HOOK_RAW(sub_82ACA290) {
  const auto user = ctx.r3.u32, address = ctx.r4.u32, length = ctx.r5.u32;
  __imp__sub_82ACA290(ctx, base);
  if (ctx.r3.u32 != 0 || user != 0 || !address || !length) return;
  const auto name = pdz_profiles::Name();
  const auto count = std::min<size_t>(name.size(), std::min(length, 16u) - 1);
  std::memcpy(base + address, name.data(), count); base[address + count] = 0;
}
REX_HOOK_RAW(sub_82AC4B18) {
  pdz_profiles::RequestSignin(); ctx.r3.u64 = 0;
}
REX_HOOK_RAW(sub_82AC4B20) {
  // XamShowSigninUIp(user, count, flags) was an empty SDK stub.
  pdz_profiles::RequestSignin(); ctx.r3.u64 = 0;
}
