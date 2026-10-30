// Field of view slider. The dialog only changes pdz_fov; the camera update
// picks the value up on the guest thread.
#include <memory>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "../game_constants.h"
#include "../game_cvars.h"
#include "fov_settings.h"

namespace
{
  using namespace GameConstants::PatchConstants;

  class FovDialog final : public rex::ui::ImGuiDialog
  {
  public:
    explicit FovDialog(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer) {}

  protected:
    void OnDraw(ImGuiIO & /*io*/) override
    {
      ImGui::SetNextWindowSize(ImVec2(480, 180), ImGuiCond_FirstUseEver);
      if (!ImGui::Begin("Field of view##pdz", nullptr, ImGuiWindowFlags_NoCollapse))
      {
        ImGui::End();
        return;
      }

      float fov = static_cast<float>(REXCVAR_GET(pdz_fov));
      if (ImGui::SliderFloat("Vertical FoV", &fov, kFovMin, kFovMax, "%.0f degrees",
                             ImGuiSliderFlags_AlwaysClamp))
      {
        REXCVAR_SET(pdz_fov, static_cast<double>(fov));
      }
      if (ImGui::Button("Reset to original (58)"))
      {
        REXCVAR_SET(pdz_fov, static_cast<double>(kFovOriginal));
      }
      ImGui::TextWrapped("Updates on the next gameplay camera frame. To keep this value, "
                         "use Save to config in the F4 Settings overlay.");
      ImGui::End();
    }
  };

  std::unique_ptr<FovDialog> g_dialog;
} // namespace

namespace fov_settings
{
  void Init(rex::ui::ImGuiDrawer *drawer)
  {
    rex::ui::RegisterBind("bind_fov_settings", "F6", "Toggle field of view slider", [drawer]
                          {
      if (g_dialog)
      {
        g_dialog.reset();
      }
      else
      {
        g_dialog = std::make_unique<FovDialog>(drawer);
      } });
  }

  bool IsOpen()
  {
    return g_dialog != nullptr;
  }

  void Shutdown()
  {
    rex::ui::UnregisterBind("bind_fov_settings");
    g_dialog.reset();
  }
} // namespace fov_settings
