#include "SlopAuras.h"

#include <GWCA/GameEntities/Skill.h>
#include <GWCA/Managers/EffectMgr.h>

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static SlopAuras instance;
    return &instance;
}

void SlopAuras::Draw(IDirect3DDevice9*)
{
    if (!GetVisiblePtr() || !*GetVisiblePtr()) {
        return;
    }

    if (ImGui::Begin(Name(), GetVisiblePtr(), GetWinFlags())) {
        const auto effects = GW::Effects::GetPlayerEffects();
        bool has_active_effect = false;

        if (effects) {
            for (const auto skill_id : effect_ids) {
                if (skill_id <= 0) {
                    continue;
                }

                DWORD remaining = 0;
                for (const auto& effect : *effects) {
                    if (static_cast<int>(effect.skill_id) == skill_id) {
                        remaining = std::max(remaining, effect.GetTimeRemaining());
                    }
                }
                if (remaining == 0) {
                    continue;
                }

                ImGui::Text("ID %d: %.1fs", skill_id, remaining / 1000.f);
                has_active_effect = true;
            }
        }

        if (!has_active_effect) {
            ImGui::TextUnformatted("No tracked effects active");
        }
    }
    ImGui::End();
}

void SlopAuras::DrawSettings()
{
    ToolboxUIPlugin::DrawSettings();

    ImGui::TextUnformatted("Track player effects by skill ID:");
    for (size_t i = 0; i < effect_ids.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(100.f);
        ImGui::InputInt("ID", &effect_ids[i], 0);
        effect_ids[i] = std::max(0, effect_ids[i]);
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            effect_ids.erase(effect_ids.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    if (ImGui::Button("Add effect ID")) {
        effect_ids.emplace_back(0);
    }
}

void SlopAuras::LoadSettings(const wchar_t* folder)
{
    ToolboxUIPlugin::LoadSettings(folder);
    LoadSetting("effect_ids", effect_ids);
    for (auto& skill_id : effect_ids) {
        skill_id = std::max(0, skill_id);
    }
}

void SlopAuras::SaveSettings(const wchar_t* folder)
{
    SaveSetting("effect_ids", effect_ids);
    ToolboxUIPlugin::SaveSettings(folder);
}
