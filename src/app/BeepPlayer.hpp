#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

class BeepPlayer {
public:
    BeepPlayer() = default;
    ~BeepPlayer();
    BeepPlayer(const BeepPlayer&) = delete;
    BeepPlayer& operator=(const BeepPlayer&) = delete;

    /**
     * @name play
     * @description Plays the proximity beep once at the given linear volume
     * (0 to 1) and pitch step on a worker thread. Ignored while a previous
     * beep is still playing or when no audio player is available.
     *
     * @example
     * beep_player.play(0.6f, plan.pitch_step);
     */
    void play(float volume, int pitch_step);

private:
    enum class Backend { Unresolved, Unavailable, PipeWire, PulseAudio };

    struct Request {
        float volume = 0.0f;
        int pitch_step = 0;
    };

    void start_worker();
    void run();
    void play_blocking(const Request& request);
    void wait_for_exit(pid_t pid);
    std::string volume_argument(float volume) const;
    void warn_once(const std::string& message);

    Backend backend_ = Backend::Unresolved;
    std::string player_path_;
    std::vector<std::string> sound_paths_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> pending_;
    bool busy_ = false;
    std::atomic<bool> stop_{false};
    bool warned_ = false;
};
