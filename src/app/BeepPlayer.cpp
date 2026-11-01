#include "BeepPlayer.hpp"
#include "ProximityBeep.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <grp.h>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

constexpr std::chrono::milliseconds kPlayTimeout{1500};
constexpr std::chrono::milliseconds kPollInterval{2};

struct UserIdentity {
    uid_t uid = 0;
    gid_t gid = 0;
    std::string home;
};

std::string find_in_path(const std::string& name)
{
    const char* path_env = std::getenv("PATH");
    if (!path_env) return {};
    const std::string path_list = path_env;
    size_t start = 0;
    while (start <= path_list.size()) {
        const size_t end = path_list.find(':', start);
        const std::string dir = path_list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            const std::string candidate = dir + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

std::optional<UserIdentity> invoking_user()
{
    if (geteuid() != 0) return std::nullopt;
    const char* uid_text = std::getenv("SUDO_UID");
    const char* gid_text = std::getenv("SUDO_GID");
    if (!uid_text || !gid_text) return std::nullopt;

    UserIdentity identity;
    identity.uid = static_cast<uid_t>(std::strtoul(uid_text, nullptr, 10));
    identity.gid = static_cast<gid_t>(std::strtoul(gid_text, nullptr, 10));
    if (identity.uid == 0) return std::nullopt;

    std::vector<char> buffer(4096);
    passwd entry{};
    passwd* found = nullptr;
    if (getpwuid_r(identity.uid, &entry, buffer.data(), buffer.size(), &found) == 0 && found) {
        identity.home = found->pw_dir;
    }
    return identity;
}

std::vector<std::string> environment_for(const UserIdentity& identity)
{
    std::vector<std::string> variables;
    for (char** entry = environ; entry && *entry; ++entry) {
        const std::string text = *entry;
        if (text.rfind("XDG_RUNTIME_DIR=", 0) == 0 || text.rfind("HOME=", 0) == 0) continue;
        variables.push_back(text);
    }
    variables.push_back("XDG_RUNTIME_DIR=/run/user/" + std::to_string(identity.uid));
    if (!identity.home.empty()) variables.push_back("HOME=" + identity.home);
    return variables;
}

}

BeepPlayer::~BeepPlayer()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void BeepPlayer::play(float volume, int pitch_step)
{
    if (backend_ == Backend::Unavailable) return;
    if (backend_ == Backend::Unresolved) {
        start_worker();
        if (backend_ == Backend::Unavailable) return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ || stop_) return;
    busy_ = true;
    pending_ = Request{std::clamp(volume, 0.0f, 1.0f), std::clamp(pitch_step, 0, proximity_beep::kPitchSteps - 1)};
    wake_.notify_one();
}

void BeepPlayer::start_worker()
{
    sound_paths_.clear();
    for (int step = 0; step < proximity_beep::kPitchSteps; ++step) {
        char name[48];
        std::snprintf(name, sizeof(name), "assets/sounds/proximity_beep_%02d.wav", step);
        sound_paths_.push_back(get_config_path(name));
        if (!std::filesystem::exists(sound_paths_.back())) {
            warn_once("Proximity beep sound not found at " + sound_paths_.back());
            backend_ = Backend::Unavailable;
            return;
        }
    }

    player_path_ = find_in_path("pw-play");
    if (!player_path_.empty()) {
        backend_ = Backend::PipeWire;
    } else {
        player_path_ = find_in_path("paplay");
        if (!player_path_.empty()) backend_ = Backend::PulseAudio;
    }
    if (player_path_.empty()) {
        warn_once("Proximity beep needs pw-play or paplay in PATH");
        backend_ = Backend::Unavailable;
        return;
    }

    worker_ = std::thread([this] { run(); });
}

void BeepPlayer::run()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [this] { return stop_.load() || pending_.has_value(); });
        if (stop_) return;

        const Request request = *pending_;
        pending_.reset();
        lock.unlock();
        play_blocking(request);
        lock.lock();
        busy_ = false;
    }
}

std::string BeepPlayer::volume_argument(float volume) const
{
    char text[32];
    if (backend_ == Backend::PipeWire) {
        std::snprintf(text, sizeof(text), "--volume=%.3f", volume);
    } else {
        std::snprintf(text, sizeof(text), "--volume=%d", static_cast<int>(std::lround(volume * 65536.0f)));
    }
    return text;
}

void BeepPlayer::play_blocking(const Request& request)
{
    if (request.volume <= 0.0f) return;

    const std::string player_name = std::filesystem::path(player_path_).filename().string();
    const std::string volume_arg = volume_argument(request.volume);
    const std::string& sound_path = sound_paths_[static_cast<size_t>(request.pitch_step)];
    char* const argv[] = {
        const_cast<char*>(player_name.c_str()),
        const_cast<char*>(volume_arg.c_str()),
        const_cast<char*>(sound_path.c_str()),
        nullptr,
    };

    const std::optional<UserIdentity> identity = invoking_user();
    std::vector<std::string> environment_storage;
    std::vector<char*> envp;
    if (identity) {
        environment_storage = environment_for(*identity);
        for (std::string& variable : environment_storage) envp.push_back(variable.data());
        envp.push_back(nullptr);
    }

    const int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) {
        if (null_fd >= 0) close(null_fd);
        warn_once("Proximity beep could not fork the audio player");
        return;
    }
    if (pid == 0) {
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        if (identity) {
            if (setgroups(0, nullptr) != 0 || setgid(identity->gid) != 0 || setuid(identity->uid) != 0) _exit(126);
            execve(player_path_.c_str(), argv, envp.data());
        } else {
            execve(player_path_.c_str(), argv, environ);
        }
        _exit(127);
    }

    if (null_fd >= 0) close(null_fd);
    wait_for_exit(pid);
}

void BeepPlayer::wait_for_exit(pid_t pid)
{
    const auto deadline = std::chrono::steady_clock::now() + kPlayTimeout;
    int status = 0;
    while (true) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                warn_once("Proximity beep audio player exited abnormally (status " + std::to_string(status) + ")");
            }
            return;
        }
        if (done < 0 && errno != EINTR) return;
        if (stop_ || std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

void BeepPlayer::warn_once(const std::string& message)
{
    if (warned_) return;
    warned_ = true;
    BUNNYWARE_LOG_WARN("{}", message);
}
