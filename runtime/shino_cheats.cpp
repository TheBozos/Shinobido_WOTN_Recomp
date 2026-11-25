// Shinobido's cheats (shino_cheats.h). The executable is stripped: the fields are the ones found by watching the game's
// memory as the ninja walks and is hurt (docs/RECOMP_NOTES.md, "Cheats"):
//
// - The player's object: the pointer at gp-0x6a08 (0x625d68; also gp-0x6308, and gp-0x6a0c, which some missions leave
//   stale). Its first word points at itself (the check that it is one, with his health and position).
// - His position: his world matrix at +0x10 (rows x, y, z, then the translation): the translation at +0x40. Written
//   there, the game moves him on from it (a copy at +0x140 and the draw's are the last frame's). The z row (+0x30) is
//   the way he faces.
// - His health: +0x64c (an int), out of +0x650 (100 in the first mission).
// - The characters: a table the pointer at gp-0x6d38 (0x625a38) gives, 16-byte entries from +0x20, the object at +4
//   (the player first, then the guards). Each character's object has the same layout as the player's; the pointer at
//   +0x45c gives its status block, which starts with the character's address again (the player's at +0x630, the
//   first guards' at +0x620, others' before the object): health at +0x1c out of +0x20 (24 for the first mission's
//   guards).
// - The guards' senses: each guard's brain (its class's table at 0x61af90, +8 the guard) keeps its stimuli in three
//   queues of 0x30-byte records (+0x394, +0x3a4, +0x3b4: the source, a state, the position at +0x10, a priority and a
//   kind at +0x20 / +0x24). Each frame (0x167540) it takes the first into its record at +0x10, then +0x40 (its target:
//   what a fight goes after). Every stimulus is pushed by 0x16b3a0: the Unseen cheat drops the player's there
//   (runtime/shino_port.cpp, the native site at 0x16b93c), and his "seen" events (0x112d60, type 17). What the guards
//   hear (the brain's noise list, +0x2e0: no source) still reaches them: they go to look, and never find him.
// - The mission's goal: the object at gp-0x6b14 (0x625c5c) holds the mission's state; its carry part at +0x440 (what
//   0x2f7b10 gives: +4 the carrier) lists at +0x88 up to 16 markers of the goal area (each an object with a world
//   matrix, its translation at +0xc0; the second mission's are the four corners of the room where the contact waits).
//   Carrying the item into that area hands it over (the script's EventItemGoal2, "Cut_Receiver").
#include "shino_cheats.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace shino::cheats
{
    namespace
    {
        // The pointers to the player's object: gp-0x6a08, gp-0x6308, gp-0x6a0c (the last is left stale by some missions'
        // loads: the third mission's points at freed memory).
        constexpr uint32_t kPlayerPointers[] = {0x625d68, 0x626468, 0x625d64};
        constexpr uint32_t kMatrix = 0x10, kFacing = 0x30, kPosition = 0x40, kHealth = 0x64c, kMaxHealth = 0x650;

        bool fromEnvironment(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e && *e != '0';
        }

        std::atomic<bool> g_on[3] = {fromEnvironment("SHINO_CHEAT_HEALTH"), fromEnvironment("SHINO_CHEAT_KILLS"),
                                     fromEnvironment("SHINO_CHEAT_UNSEEN")};
        constexpr uint32_t kCharacterTable = 0x625a38; // gp-0x6d38
        constexpr uint32_t kMaxCharacters = 128;
        constexpr uint32_t kStatus = 0x45c; // a character's status block (its health at +0x1c, out of +0x20)

        // The teleport asked for (by the tab or SHINO_TELEPORT), made at the next frame the player is in play.
        enum class Kind { None, To, By, Goal, Item, Enemy, Character, Complete };
        struct Request
        {
            Kind kind = Kind::None;
            float a = 0, b = 0, c = 0;
        };
        std::mutex g_lock; // the request, the slots, the status, the player's last state
        Request g_request;
        Player g_player;
        Player g_slots[kSlots];
        Place g_goal, g_item, g_enemy;
        struct Waypoint
        {
            float x, z, step;
            bool fly = false;
            float y = 0;
        };
        std::vector<Waypoint> g_walk;
        float g_enemyFacing[2] = {0, 1};
        constexpr uint32_t kMissionPointer = 0x625c5c; // gp-0x6b14
        constexpr uint32_t kCarry = 0x440, kGoalMarkers = 0x88, kMarkerPosition = 0xc0;
        constexpr uint32_t kItemList = 0x625ac8; // gp-0x6ca8

        std::string g_status;

        void setStatus(const std::string &s)
        {
            g_status = s;
            std::printf("[shino:cheats] %s\n", s.c_str());
        }

        uint32_t read32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x1FFFFFFu), 4);
            return v;
        }
        float readF(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & 0x1FFFFFFu), 4);
            return v;
        }
        void writeF(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & 0x1FFFFFFu), &v, 4); }

        // Where a character's health is (its max 4 bytes after): +0x64c, else the status block's +0x1c; 0: neither.
        uint32_t healthAt(const uint8_t *ram, uint32_t object)
        {
            const auto valid = [&](uint32_t a) {
                const int32_t health = static_cast<int32_t>(read32(ram, a)), max = static_cast<int32_t>(read32(ram, a + 4));
                return max > 0 && max <= 100000 && health >= 0 && health <= max;
            };
            if (valid(object + kHealth))
                return object + kHealth;
            const uint32_t block = read32(ram, object + 0x45c);
            if (block >= 0x100000u && block < 0x2000000u - 0x40u && read32(ram, block) == object && valid(block + 0x1c))
                return block + 0x1c;
            return 0;
        }

        bool isPlayer(const uint8_t *ram, uint32_t object)
        {
            if (object < 0x100000u || object >= 0x2000000u - 0x1000u || read32(ram, object) != object)
                return false;
            // health at +0x64c, or (some phases, e.g. the Kabuto fight's second part) only in the status block
            const uint32_t at = healthAt(ram, object);
            if (!at)
                return false;
            for (uint32_t k = 0; k < 3; ++k)
                if (!std::isfinite(readF(ram, object + kPosition + 4 * k)))
                    return false;
            return true;
        }
        uint32_t playerObject(const uint8_t *ram)
        {
            for (const uint32_t pointer : kPlayerPointers)
                if (const uint32_t object = read32(ram, pointer); isPlayer(ram, object))
                    return object;
            return 0;
        }

        // A world matrix's translation (+at), if it is one (w 1, finite, in the world).
        bool translation(const uint8_t *ram, uint32_t object, uint32_t at, float out[3])
        {
            if (object < 0x100000u || object >= 0x2000000u - 0x200u)
                return false;
            float v[4];
            for (uint32_t k = 0; k < 4; ++k)
                v[k] = readF(ram, object + at + 4 * k);
            if (v[3] != 1.0f)
                return false;
            for (uint32_t k = 0; k < 3; ++k)
                if (!std::isfinite(v[k]) || std::fabs(v[k]) > 1e5f)
                    return false;
            std::memcpy(out, v, sizeof(float) * 3);
            return true;
        }
        float distance2(const float a[3], const float b[3])
        {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        // The goal: the mission state's carry part (+0x440) lists the goal markers at +0x88 (their translation at
        // +0xc0). Close together (within 80 units of their centre) they are one area: its centre; else the nearest.
        Place findGoal(const uint8_t *ram, const float at[3])
        {
            Place g;
            const uint32_t mission = read32(ram, kMissionPointer);
            if (mission < 0x100000u || mission >= 0x2000000u - 0x1000u)
                return g;
            float markers[16][3], centre[3] = {0, 0, 0};
            int n = 0;
            for (uint32_t i = 0; i < 16; ++i)
                if (translation(ram, read32(ram, mission + kCarry + kGoalMarkers + 4 * i), kMarkerPosition, markers[n]))
                {
                    for (uint32_t k = 0; k < 3; ++k)
                        centre[k] += markers[n][k];
                    ++n;
                }
            if (!n)
                return g;
            for (uint32_t k = 0; k < 3; ++k)
                centre[k] /= n;
            bool together = true;
            for (int i = 0; i < n; ++i)
                together = together && distance2(markers[i], centre) <= 80.0f * 80.0f;
            const float *best = centre;
            if (!together)
                for (int i = 0; i < n; ++i)
                    if (best == centre || distance2(markers[i], at) < distance2(best, at))
                        best = markers[i];
            g = {true, best[0], best[1], best[2], n};
            return g;
        }

        // The item to take: the carriable items' list (the object gp-0x6ca8 gives, its list at +0x3c / +0x40: each
        // node's +8 an item, its translation at +0xc0), the nearest one more than 3 units from the ninja (the one he
        // carries moves with him).
        Place findItem(const uint8_t *ram, const float at[3])
        {
            Place p;
            const uint32_t list = read32(ram, kItemList);
            if (list < 0x100000u || list >= 0x2000000u - 0x100u)
                return p;
            float best[3], v[3], bestD = 0;
            uint32_t node = read32(ram, list + 0x40);
            for (int guard = 0; guard < 256 && node != list + 0x3c && node >= 0x100000u && node < 0x2000000u; ++guard)
            {
                if (translation(ram, read32(ram, node + 8), kMarkerPosition, v))
                {
                    const float d = distance2(v, at);
                    ++p.count;
                    if (d > 9.0f && (!p.valid || d < bestD))
                    {
                        p.valid = true;
                        bestD = d;
                        std::memcpy(best, v, sizeof best);
                    }
                }
                node = read32(ram, node + 4);
            }
            if (p.valid)
                p.x = best[0], p.y = best[1], p.z = best[2];
            return p;
        }

        // A character's status block: the pointer at +0x45c (inside the object for some, the player's at +0x630, the
        // first guards' at +0x620; before it for others, at -0x35d0 or -0x7190), the block starting with the
        // character's address: its address, 0 when there is none valid.
        uint32_t statusBlock(const uint8_t *ram, uint32_t object)
        {
            const uint32_t block = read32(ram, object + kStatus);
            if (block < 0x100000u || block >= 0x2000000u - 0x40u || read32(ram, block) != object)
                return 0;
            const int32_t health = static_cast<int32_t>(read32(ram, block + 0x1c));
            const int32_t max = static_cast<int32_t>(read32(ram, block + 0x20));
            return max > 0 && max <= 100000 && health >= 0 && health <= max ? block : 0;
        }
        template <class F> void forEachEnemy(const uint8_t *ram, uint32_t player, F &&f)
        {
            const uint32_t table = read32(ram, kCharacterTable);
            if (table < 0x100000u || table >= 0x2000000u - kMaxCharacters * 16u)
                return;
            for (uint32_t i = 0; i < kMaxCharacters; ++i)
            {
                const uint32_t object = read32(ram, table + 0x20 + 16 * i + 4);
                if (object == player || object < 0x100000u || object >= 0x2000000u - 0x1000u || read32(ram, object) != object)
                    continue;
                if (const uint32_t block = statusBlock(ram, object))
                    f(object, block + 0x1c);
            }
        }

        // SHINO_TELEPORT=<x>,<y>,<z>: a teleport asked for at the start (runs without a window).
        void teleportFromEnvironment()
        {
            const char *e = std::getenv("SHINO_TELEPORT");
            float v[3];
            if (e && std::sscanf(e, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3)
                g_request = {Kind::To, v[0], v[1], v[2]};
        }
    } // namespace

    bool enabled(Cheat cheat) { return g_on[static_cast<int>(cheat)].load(); }

    bool hidesSource(const uint8_t *ram, uint32_t source, uint32_t position)
    {
        if (!enabled(Cheat::Unseen))
            return false;
        const uint32_t player = playerObject(ram);
        return player && (source == player || (source == 0 && position == player + kPosition));
    }

    void setEnabled(Cheat cheat, bool on) { g_on[static_cast<int>(cheat)] = on; }

    Player player()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_player;
    }

    void teleportTo(float x, float y, float z)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_request = {Kind::To, x, y, z};
    }
    void walkTo(float x, float z, float step)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_walk.push_back({x, z, step > 0 ? step : 0.2f});
    }
    void flyTo(float x, float y, float z, float step)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_walk.push_back({x, z, step > 0 ? step : 0.2f, true, y});
    }
    void teleportBy(float forward, float up)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_request = {Kind::By, forward, up, 0};
    }
    Place goal()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_goal;
    }
    Place item()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_item;
    }
    Place enemy()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_enemy;
    }
    void teleportToGoal()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_request = {Kind::Goal, 0, 0, 0};
    }
    void teleportToItem()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_request = {Kind::Item, 0, 0, 0};
    }
    void teleportToEnemy()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_request = {Kind::Enemy, 0, 0, 0};
    }
    void completeMission()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_request = {Kind::Complete, 0, 0, 0};
    }
    void teleportToCharacter(int index)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_request = {Kind::Character, static_cast<float>(index), 0, 0};
    }
    void savePosition(int slot)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (slot < 0 || slot >= kSlots)
            return;
        if (!g_player.valid)
        {
            setStatus("Nothing saved: the ninja is not in play.");
            return;
        }
        g_slots[slot] = g_player;
        char text[96];
        std::snprintf(text, sizeof text, "Slot %d: %.1f, %.1f, %.1f saved.", slot + 1, g_player.x, g_player.y, g_player.z);
        setStatus(text);
    }
    bool hasSavedPosition(int slot)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return slot >= 0 && slot < kSlots && g_slots[slot].valid;
    }
    void loadPosition(int slot)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (slot >= 0 && slot < kSlots && g_slots[slot].valid)
            g_request = {Kind::To, g_slots[slot].x, g_slots[slot].y, g_slots[slot].z};
    }
    std::string status()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_status;
    }

    void apply(uint8_t *ram)
    {
        static const bool once = [] { teleportFromEnvironment(); return true; }();
        (void)once;
        const uint32_t object = playerObject(ram);
        std::lock_guard<std::mutex> lock(g_lock);
        if (!object)
        {
            g_goal = g_item = g_enemy = {};
            g_player.valid = false;
            return;
        }
        if (enabled(Cheat::InfiniteHealth))
            if (const uint32_t at = healthAt(ram, object))
                std::memcpy(ram + at, ram + at + 4, 4);
        int enemies = 0, alive = 0;
        const bool kills = enabled(Cheat::OneHitKills);
        float here[3];
        for (uint32_t k = 0; k < 3; ++k)
            here[k] = readF(ram, object + kPosition + 4 * k);
        g_goal = findGoal(ram, here);
        g_item = findItem(ram, here);
        g_enemy = {};
        float enemyD = 0;
        char roster[2048];
        int rosterAt = 0;
        forEachEnemy(ram, object, [&](uint32_t guard, uint32_t healthAt) {
            ++enemies;
            const int32_t health = static_cast<int32_t>(read32(ram, healthAt));
            if (rosterAt < static_cast<int>(sizeof roster) - 64) // SHINO_TRACE_ENEMIES=2: each one (object, health / max)
                rosterAt += std::snprintf(roster + rosterAt, sizeof roster - rosterAt, " %x:%d/%d@%.0f,%.0f,%.0f", guard, health,
                                          static_cast<int32_t>(read32(ram, healthAt + 4)), readF(ram, guard + kPosition),
                                          readF(ram, guard + kPosition + 4), readF(ram, guard + kPosition + 8));
            // SHINO_KILL_SPARE=<max health>: the enemies with that much (a rival ninja: the Mosu have 120) are left alone
            static const int spare = std::getenv("SHINO_KILL_SPARE") ? std::atoi(std::getenv("SHINO_KILL_SPARE")) : 0;
            if (spare && static_cast<int32_t>(read32(ram, healthAt + 4)) == spare)
                return;
            if (health > 0)
            {
                ++alive;
                float at[3];
                for (uint32_t k = 0; k < 3; ++k)
                    at[k] = readF(ram, guard + kPosition + 4 * k);
                if (const float d = distance2(at, here); std::isfinite(d) && (!g_enemy.valid || d < enemyD))
                {
                    enemyD = d;
                    g_enemy = {true, at[0], at[1], at[2], 0};
                    g_enemyFacing[0] = readF(ram, guard + kFacing);
                    g_enemyFacing[1] = readF(ram, guard + kFacing + 8);
                }
            }
            if (kills && health > 1) // the dead (0) left alone: the game ends them
            {
                const int32_t one = 1;
                std::memcpy(ram + healthAt, &one, 4);
            }
        });
        if (!g_walk.empty())
        {
            const Waypoint w = g_walk.front();
            const float x = readF(ram, object + kPosition), z = readF(ram, object + kPosition + 8);
            // SHINO_WALK_SINK=<units a frame>: off a ledge he comes down no faster than that (as down a slope)
            static const float sink = std::getenv("SHINO_WALK_SINK") ? static_cast<float>(std::atof(std::getenv("SHINO_WALK_SINK"))) : 0.0f;
            static float lastY = 0.0f;
            static bool haveY = false;
            if (sink > 0.0f)
            {
                const float y = readF(ram, object + kPosition + 4);
                if (haveY && y < lastY - sink)
                    writeF(ram, object + kPosition + 4, lastY - sink);
                lastY = haveY && y < lastY - sink ? lastY - sink : y;
                haveY = true;
            }
            const float y0 = readF(ram, object + kPosition + 4);
            float dx = w.x - x, dz = w.z - z, dy = w.fly ? w.y - y0 : 0.0f;
            const float length = std::sqrt(dx * dx + dz * dz + dy * dy);
            if (length <= w.step)
            {
                writeF(ram, object + kPosition, w.x);
                writeF(ram, object + kPosition + 8, w.z);
                if (w.fly)
                    writeF(ram, object + kPosition + 4, w.y);
                g_walk.erase(g_walk.begin());
                if (g_walk.empty())
                    setStatus("Walk over.");
            }
            else
            {
                dx /= length, dz /= length, dy /= length;
                writeF(ram, object + kPosition, x + dx * w.step);
                writeF(ram, object + kPosition + 8, z + dz * w.step);
                if (w.fly)
                    writeF(ram, object + kPosition + 4, y0 + dy * w.step);
                const float h = std::sqrt(dx * dx + dz * dz);
                if (h > 1e-4f)
                    dx /= h, dz /= h;
                const float rows[3][3] = {{dz, 0, -dx}, {0, 1, 0}, {dx, 0, dz}};
                for (uint32_t r = 0; r < 3; ++r)
                    for (uint32_t k = 0; k < 3; ++k)
                        writeF(ram, object + kMatrix + 16 * r + 4 * k, rows[r][k]);
            }
        }
        if (g_request.kind != Kind::None)
        {
            float to[3] = {0, 0, 0};
            bool go = true;
            if (g_request.kind == Kind::Complete)
            {
                const uint32_t mission = read32(ram, kMissionPointer);
                if (mission >= 0x100000u && mission < 0x2000000u - 0x1000u)
                {
                    ram[(mission + kCarry + 0x16u) & 0x1FFFFFFu] = 1;
                    ram[(mission + kCarry + 0x17u) & 0x1FFFFFFu] = 1;
                    setStatus("Mission marked as completed.");
                }
                go = false;
            }
            else if (g_request.kind == Kind::Character)
            {
                const uint32_t table = read32(ram, kCharacterTable);
                const uint32_t index = static_cast<uint32_t>(g_request.a);
                const uint32_t who = table >= 0x100000u && index < kMaxCharacters ? read32(ram, table + 0x20 + 16 * index + 4) : 0;
                if (who < 0x100000u || who >= 0x2000000u - 0x1000u || read32(ram, who) != who)
                {
                    setStatus("No such character.");
                    go = false;
                }
                else
                {
                    float fx = readF(ram, who + kFacing), fz = readF(ram, who + kFacing + 8);
                    const float length = std::sqrt(fx * fx + fz * fz);
                    if (length > 1e-4f)
                        fx /= length, fz /= length;
                    to[0] = readF(ram, who + kPosition) - fx * 6.0f;
                    to[1] = readF(ram, who + kPosition + 4) + 0.5f;
                    to[2] = readF(ram, who + kPosition + 8) - fz * 6.0f;
                }
            }
            else if (g_request.kind == Kind::Goal || g_request.kind == Kind::Item || g_request.kind == Kind::Enemy)
            {
                const Place &place = g_request.kind == Kind::Goal ? g_goal : g_request.kind == Kind::Item ? g_item : g_enemy;
                if (!place.valid)
                {
                    setStatus(g_request.kind == Kind::Goal   ? "No goal: this mission has no goal markers (or not yet)."
                              : g_request.kind == Kind::Item ? "No item to take in this mission (or it is carried)."
                                                             : "No enemy left alive.");
                    go = false;
                }
                to[0] = place.x;
                to[1] = place.y + 1.5f; // a little above the markers' floor, the item's or the enemy's feet
                to[2] = place.z;
                if (g_request.kind == Kind::Item) // just short of it, facing it (Grab takes what is in front of him)
                {
                    float fx = readF(ram, object + kFacing), fz = readF(ram, object + kFacing + 8);
                    const float length = std::sqrt(fx * fx + fz * fz);
                    if (length > 1e-4f)
                        fx /= length, fz /= length;
                    else
                        fx = 1.0f, fz = 0.0f;
                    to[0] -= fx * 1.4f;
                    to[1] = place.y + 0.5f;
                    to[2] -= fz * 1.4f;
                }
                if (g_request.kind == Kind::Enemy) // just behind him, the way he faces
                {
                    float fx = g_enemyFacing[0], fz = g_enemyFacing[1];
                    const float length = std::sqrt(fx * fx + fz * fz);
                    if (length > 1e-4f)
                        fx /= length, fz /= length;
                    to[0] -= fx * 2.0f;
                    to[1] = place.y + 0.5f;
                    to[2] -= fz * 2.0f;
                }
            }
            else if (g_request.kind == Kind::To)
            {
                to[0] = g_request.a;
                to[1] = g_request.b;
                to[2] = g_request.c;
            }
            else // forward along the way he faces (his z row, on the ground), and up
            {
                float fx = readF(ram, object + kFacing), fz = readF(ram, object + kFacing + 8);
                const float length = std::sqrt(fx * fx + fz * fz);
                if (length > 1e-4f)
                    fx /= length, fz /= length;
                to[0] = readF(ram, object + kPosition) + fx * g_request.a;
                to[1] = readF(ram, object + kPosition + 4) + g_request.b;
                to[2] = readF(ram, object + kPosition + 8) + fz * g_request.a;
            }
            if (go)
            {
                for (uint32_t k = 0; k < 3; ++k)
                    writeF(ram, object + kPosition + 4 * k, to[k]);
                if (g_request.kind == Kind::Item) // turned toward it: his matrix's rows x (+0x10), y, z (+0x30, forward)
                {
                    float dx = g_item.x - to[0], dz = g_item.z - to[2];
                    const float length = std::sqrt(dx * dx + dz * dz);
                    if (length > 1e-3f)
                    {
                        dx /= length, dz /= length;
                        const float rows[3][3] = {{dz, 0, -dx}, {0, 1, 0}, {dx, 0, dz}};
                        for (uint32_t r = 0; r < 3; ++r)
                            for (uint32_t k = 0; k < 3; ++k)
                                writeF(ram, object + kMatrix + 16 * r + 4 * k, rows[r][k]);
                    }
                }
                char text[96];
                std::snprintf(text, sizeof text, "Teleported to %.1f, %.1f, %.1f.", to[0], to[1], to[2]);
                setStatus(text);
            }
            g_request = {};
        }
        // SHINO_TRACE_ENEMIES=1: the enemies alive, each time it changes (play tests of the kill missions).
        // (and the carriable items: a soul fragment a rival ninja drops is one)
        static int s_items = -1;
        if (static const bool trace = fromEnvironment("SHINO_TRACE_ENEMIES"); trace && (alive != g_player.enemiesAlive ||
                                                                                       enemies != g_player.enemies ||
                                                                                       g_item.count != s_items))
        {
            std::printf("[shino:cheats] enemies %d, %d alive, items %d\n", enemies, alive, g_item.count);
            if (static const bool each = std::getenv("SHINO_TRACE_ENEMIES")[0] == '2'; each)
                std::printf("[shino:cheats]  %.*s\n", rosterAt, roster);
        }
        s_items = g_item.count;
        // SHINO_TRACE_PLAYER=<n>: the ninja's position every n frames (and the mission's carrier: 0 none)
        if (static const int every = std::getenv("SHINO_TRACE_PLAYER") ? std::atoi(std::getenv("SHINO_TRACE_PLAYER")) : 0; every > 0)
        {
            static int frame = 0;
            if (++frame % every == 0)
            {
                const uint32_t mission = read32(ram, kMissionPointer);
                const uint32_t carrier = mission >= 0x100000u && mission < 0x2000000u - 0x1000u ? read32(ram, mission + kCarry + 4) : 0;
                std::printf("[shino:player] %.1f %.1f %.1f carrier %x motion %u%c", readF(ram, object + kPosition), readF(ram, object + kPosition + 4),
                            readF(ram, object + kPosition + 8), carrier, read32(ram, 0x6711b4u), 10);
            }
        }
        Player &p = g_player;
        p.valid = true;
        p.x = readF(ram, object + kPosition);
        p.y = readF(ram, object + kPosition + 4);
        p.z = readF(ram, object + kPosition + 8);
        p.fx = readF(ram, object + kFacing);
        p.fz = readF(ram, object + kFacing + 8);
        p.health = static_cast<int32_t>(read32(ram, healthAt(ram, object)));
        p.maxHealth = static_cast<int32_t>(read32(ram, healthAt(ram, object) + 4));
        p.enemies = enemies;
        p.enemiesAlive = alive;
    }
} // namespace shino::cheats
