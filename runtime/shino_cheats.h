// Shinobido's cheats: toggles and teleports the player sets in the window's Cheats tab (F2, runtime/app/
// shino_cheats_ui.cpp) or, for runs without a window, from the environment (SHINO_CHEAT_HEALTH=1,
// SHINO_CHEAT_KILLS=1, SHINO_CHEAT_UNSEEN=1, SHINO_TELEPORT=<x>,<y>,<z>). Applied on the game's thread at each frame's end (runtime/shino_main.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace shino::cheats
{
    enum class Cheat
    {
        InfiniteHealth, // Goh's health kept at its maximum
        OneHitKills,    // every other character's health kept at 1: the next hit kills (the game's own death)
        Unseen,         // nothing of the ninja's reaches a guard's brain as a target (they never spot him; they still hear)
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
    // A walk: each frame the ninja is moved `step` units toward the next waypoint (x, z) and turned to face it, his
    // height left to the game (he keeps what he carries, as when he walks). Waypoints are queued.
    void walkTo(float x, float z, float step);
    void savePosition(int slot);
    bool hasSavedPosition(int slot);
    void loadPosition(int slot);
    // Places a mission has (runtime/shino_cheats.cpp), found each frame in play; count: how many it found.
    // - goal: where the mission ends: the centre of the goal markers when they are close together (a carry mission's
    //   room), else the marker nearest the ninja (a thievery mission's exits, a way out once the item is taken);
    // - item: the item to take (the carriable item nearest the ninja that he does not carry: the documents to steal);
    // - enemy: the living enemy nearest the ninja (teleportToEnemy puts him just behind it: a stealth kill's place).
    struct Place
    {
        bool valid = false;
        float x = 0, y = 0, z = 0;
        int count = 0;
    };
    using Goal = Place;
    Place goal();
    Place item();
    Place enemy();
    void teleportToGoal();
    void teleportToItem();
    void teleportToEnemy();
    // Marks the mission as completed (its completion flags, the mission state's +0x440 part's +0x16 and +0x17, as the
    // game sets them when an objective is met): the game then plays "Mission Complete!" and its rating. For play tests
    // past a mission whose objective is not understood yet (the reconnaissance missions).
    void completeMission();
    // To the character at that index of the character table (0 the player), a few units behind it: following one.
    void teleportToCharacter(int index);
    // What happened last (a teleport made, refused), for the tab.
    std::string status();

    // The Unseen cheat (runtime/shino_port.cpp): whether what a guard senses about `source` is dropped: a stimulus for
    // his brain (the stimulus push 0x16b3a0, `position` 0) or a "seen" event (the character event 0x112d60, type 17,
    // `position` the address of where): the player's, or his position with no source.
    bool hidesSource(const uint8_t *ram, uint32_t source, uint32_t position);

    // Once a frame, on the game thread (the frame boundary): the enabled cheats' writes, the teleport asked for.
    void apply(uint8_t *ram);
    // The window's Cheats tab (only in the window build).
    void registerMenu();
} // namespace shino::cheats
