// Shinobido's cheats (shino_cheats.h). The executable is stripped: the fields are the ones found by watching the game's
// memory as the ninja walks and is hurt (docs/RECOMP_NOTES.md, "Cheats"):
//
// - The player's object: the pointer at gp-0x6a0c (0x625d64; also gp-0x6a08 and gp-0x6308). Its first word points at
//   itself (the check that it is one).
// - His position: his world matrix at +0x10 (rows x, y, z, then the translation): the translation at +0x40. Written
//   there, the game moves him on from it (a copy at +0x140 and the draw's are the last frame's). The z row (+0x30) is
//   the way he faces.
// - His health: +0x64c (an int), out of +0x650 (100 in the first mission).
#include "shino_cheats.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace shino::cheats
{
    namespace
    {
        constexpr uint32_t kPlayerPointer = 0x625d64; // gp-0x6a0c
        constexpr uint32_t kMatrix = 0x10, kFacing = 0x30, kPosition = 0x40, kHealth = 0x64c, kMaxHealth = 0x650;

        bool fromEnvironment(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e && *e != '0';
        }

        std::atomic<bool> g_on[1] = {fromEnvironment("SHINO_CHEAT_HEALTH")};

        // The teleport asked for (by the tab or SHINO_TELEPORT), made at the next frame the player is in play.
        enum class Kind { None, To, By };
        struct Request
        {
            Kind kind = Kind::None;
            float a = 0, b = 0, c = 0;
        };
        std::mutex g_lock; // the request, the slots, the status, the player's last state
        Request g_request;
        Player g_player;
        Player g_slots[kSlots];
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

        // The player's object, or 0 while there is none (menus, movies, loads).
        uint32_t playerObject(const uint8_t *ram)
        {
            const uint32_t object = read32(ram, kPlayerPointer);
            if (object < 0x100000u || object >= 0x2000000u - 0x1000u || read32(ram, object) != object)
                return 0;
            const int32_t health = static_cast<int32_t>(read32(ram, object + kHealth));
            const int32_t max = static_cast<int32_t>(read32(ram, object + kMaxHealth));
            if (max <= 0 || max > 100000 || health < 0 || health > max)
                return 0;
            for (uint32_t k = 0; k < 3; ++k)
                if (!std::isfinite(readF(ram, object + kPosition + 4 * k)))
                    return 0;
            return object;
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
    void teleportBy(float forward, float up)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_request = {Kind::By, forward, up, 0};
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
            g_player.valid = false;
            return;
        }
        if (enabled(Cheat::InfiniteHealth))
            std::memcpy(ram + object + kHealth, ram + object + kMaxHealth, 4);
        if (g_request.kind != Kind::None)
        {
            float to[3];
            if (g_request.kind == Kind::To)
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
            for (uint32_t k = 0; k < 3; ++k)
                writeF(ram, object + kPosition + 4 * k, to[k]);
            char text[96];
            std::snprintf(text, sizeof text, "Teleported to %.1f, %.1f, %.1f.", to[0], to[1], to[2]);
            setStatus(text);
            g_request = {};
        }
        Player &p = g_player;
        p.valid = true;
        p.x = readF(ram, object + kPosition);
        p.y = readF(ram, object + kPosition + 4);
        p.z = readF(ram, object + kPosition + 8);
        p.fx = readF(ram, object + kFacing);
        p.fz = readF(ram, object + kFacing + 8);
        p.health = static_cast<int32_t>(read32(ram, object + kHealth));
        p.maxHealth = static_cast<int32_t>(read32(ram, object + kMaxHealth));
        (void)kMatrix;
    }
} // namespace shino::cheats
