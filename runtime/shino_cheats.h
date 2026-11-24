// Shinobido's cheats: toggles and teleports the player sets in the window's Cheats tab (F2, runtime/app/
// shino_cheats_ui.cpp) or, for runs without a window, from the environment (SHINO_CHEAT_HEALTH=1,
// SHINO_CHEAT_KILLS=1, SHINO_TELEPORT=<x>,<y>,<z>). Applied on the game's thread at each frame's end (runtime/shino_main.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace shino::cheats
{
    enum class Cheat
    {
        InfiniteHealth, // Goh's health kept at its maximum
        OneHitKills,    // every other character's health kept at 1: the next hit kills (the game's own death)
    };
    bool enabled(Cheat cheat);
    void setEnabled(Cheat cheat, bool on);

    // The player as the last frame left him (valid: in play, his object found).
    struct Player
    {
        bool valid = false;
        float x = 0, y = 0, z = 0;    // world position (y up; the ground of the castle grounds is about 0)
        float fx = 0, fz = 1;         // the way he faces, on the ground
        int32_t health = 0, maxHealth = 0;
        int enemies = 0, enemiesAlive = 0; // the other characters of the mission (the character table's)
    };
    Player player();

    // Teleports, made at the next frame's end while the player is in play: to a point, by a distance forward (the
    // way he faces) or up, or to a saved position (kSlots of them, kept for the session).
    constexpr int kSlots = 4;
    void teleportTo(float x, float y, float z);
    void teleportBy(float forward, float up);
    void savePosition(int slot);
    bool hasSavedPosition(int slot);
    void loadPosition(int slot);
    // What happened last (a teleport made, refused), for the tab.
    std::string status();

    // Once a frame, on the game thread (the frame boundary): the enabled cheats' writes, the teleport asked for.
    void apply(uint8_t *ram);
    // The window's Cheats tab (only in the window build).
    void registerMenu();
} // namespace shino::cheats
