#pragma once
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pairs the local kill counter with the enemy it killed (Kill FX). The reader drops a dead
// enemy from the packet and reads the round kill counter at different moments of its cycle,
// so either can reach the app a frame or more before the other. Each side waits here for a
// short window, and the victim's last drawable state is kept with its death.
namespace killfx {

// Both happen on the same game tick; this covers reader skew and frame pacing.
inline constexpr double kVictimMatchWindowSeconds = 0.5;

template <class Snapshot>
class KillVictimMatcher {
public:
    struct Match {
        std::string name;
        Snapshot victim;
        bool headshot = false;
    };

    // An enemy that left the packet alive (or was seen at 0 HP) at `now`.
    void record_death(std::string name, Snapshot victim, double now)
    {
        forget(name);
        deaths_.push_back({std::move(name), std::move(victim), now});
    }

    // An enemy present again was not killed (a dropped read) or has respawned.
    void forget(std::string_view name)
    {
        for (auto it = deaths_.begin(); it != deaths_.end();) it = it->name == name ? deaths_.erase(it) : it + 1;
    }

    bool waiting(std::string_view name) const
    {
        for (const auto& death : deaths_)
            if (death.name == name) return true;
        return false;
    }

    // `kills` new kills at `now`, the first `headshots` of them headshots.
    void record_kills(int kills, int headshots, double now)
    {
        for (int k = 0; k < kills; ++k) kills_.push_back({now, k < headshots});
    }

    // Pairs waiting kills with waiting deaths, oldest kill first, each with the death nearest
    // to it in time, then drops what is older than the window. A kill without a death in
    // reach keeps waiting until it expires.
    std::vector<Match> take(double now, double window = kVictimMatchWindowSeconds)
    {
        std::vector<Match> matched;
        for (auto kill = kills_.begin(); kill != kills_.end();) {
            auto best = deaths_.end();
            double best_gap = window;
            for (auto death = deaths_.begin(); death != deaths_.end(); ++death) {
                const double gap = std::fabs(death->time - kill->time);
                if (gap <= best_gap) {
                    best_gap = gap;
                    best = death;
                }
            }
            if (best == deaths_.end()) {
                ++kill;
                continue;
            }
            matched.push_back({std::move(best->name), std::move(best->victim), kill->headshot});
            deaths_.erase(best);
            kill = kills_.erase(kill);
        }
        for (auto it = kills_.begin(); it != kills_.end();) it = now - it->time > window ? kills_.erase(it) : it + 1;
        for (auto it = deaths_.begin(); it != deaths_.end();) it = now - it->time > window ? deaths_.erase(it) : it + 1;
        return matched;
    }

    void clear()
    {
        deaths_.clear();
        kills_.clear();
    }
    std::size_t pending_deaths() const { return deaths_.size(); }
    std::size_t pending_kills() const { return kills_.size(); }

private:
    struct Death {
        std::string name;
        Snapshot victim;
        double time;
    };
    struct Kill {
        double time;
        bool headshot;
    };
    std::vector<Death> deaths_;
    std::vector<Kill> kills_;
};

} // namespace killfx
