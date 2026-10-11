#pragma once
#include <filesystem>
#include <memory>
#include <rex/ui/imgui_dialog.h>

namespace pdz_profiles {
void Configure(std::filesystem::path& user_data_root);
std::unique_ptr<rex::ui::ImGuiDialog> MakeProfileDialog(rex::ui::ImGuiDrawer* drawer);
void Toggle();
bool IsOpen();
}
