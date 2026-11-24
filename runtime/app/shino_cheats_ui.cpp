// The window's Cheats tab (F2, beside Controls and Video): ImGui widgets for shino_cheats.h. Only in the window build.
#include "shino_cheats.h"

#include "bozo/app/settings_tabs.h"
#include "imgui.h"

#include <string>

namespace shino::cheats
{
    void registerMenu()
    {
        bozo::app::ui::registerTab("Cheats", [] {
            bool health = enabled(Cheat::InfiniteHealth);
            if (ImGui::Checkbox("Infinite health", &health))
                setEnabled(Cheat::InfiniteHealth, health);
            ImGui::TextDisabled("Keeps the ninja's health at its maximum.");
            bool kills = enabled(Cheat::OneHitKills);
            if (ImGui::Checkbox("One-hit kills", &kills))
                setEnabled(Cheat::OneHitKills, kills);
            ImGui::TextDisabled("Every guard's health is kept at 1: the next hit kills him (the game's own death).");
            bool unseen = enabled(Cheat::Unseen);
            if (ImGui::Checkbox("Guards never spot the ninja", &unseen))
                setEnabled(Cheat::Unseen, unseen);
            ImGui::TextDisabled("Nothing of his reaches a guard as a target. They still hear his noises and go to look.");

            ImGui::SeparatorText("Teleport");
            const Player p = player();
            if (p.valid)
                ImGui::Text("Now: %.1f, %.1f, %.1f (health %d / %d); guards %d, %d alive", p.x, p.y, p.z, p.health,
                            p.maxHealth, p.enemies, p.enemiesAlive);
            else
                ImGui::TextDisabled("The ninja is not in play (a menu, a movie, a load).");
            static float target[3] = {0, 0, 0};
            if (ImGui::Button("Here") && p.valid)
            {
                target[0] = p.x;
                target[1] = p.y;
                target[2] = p.z;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(300);
            ImGui::InputFloat3("##target", target, "%.1f");
            ImGui::SameLine();
            if (ImGui::Button("Go"))
                teleportTo(target[0], target[1], target[2]);
            static float step = 50.0f;
            ImGui::SetNextItemWidth(120);
            ImGui::InputFloat("##step", &step, 10.0f, 50.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("Forward"))
                teleportBy(step, 0.0f);
            ImGui::SameLine();
            if (ImGui::Button("Back"))
                teleportBy(-step, 0.0f);
            ImGui::SameLine();
            if (ImGui::Button("Up"))
                teleportBy(0.0f, step);
            ImGui::TextDisabled("Forward and back go the way the ninja faces; up lifts him (he falls back, onto a roof if one is under him).");
            const Goal g = goal();
            ImGui::BeginDisabled(!g.valid);
            if (ImGui::Button("To the mission's goal"))
                teleportToGoal();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (g.valid)
                ImGui::Text("%.1f, %.1f, %.1f (%d markers)", g.x, g.y, g.z, g.markers);
            else
                ImGui::TextDisabled("No goal area in this mission (yet).");

            ImGui::SeparatorText("Saved positions");
            for (int slot = 0; slot < kSlots; ++slot)
            {
                ImGui::PushID(slot);
                if (ImGui::Button("Save"))
                    savePosition(slot);
                ImGui::SameLine();
                ImGui::BeginDisabled(!hasSavedPosition(slot));
                if (ImGui::Button("Go back"))
                    loadPosition(slot);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::Text("Slot %d", slot + 1);
                ImGui::PopID();
            }
            ImGui::TextDisabled("A teleport is made at the next frame in play; the camera follows a moment later.");
            const std::string s = status();
            if (!s.empty())
                ImGui::TextWrapped("%s", s.c_str());
        });
    }
} // namespace shino::cheats
