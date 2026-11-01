#pragma once

#include <memory>
#include <string>

class FocusMonitor {
public:
    explicit FocusMonitor(bool wayland_session);
    ~FocusMonitor();

    FocusMonitor(const FocusMonitor&) = delete;
    FocusMonitor& operator=(const FocusMonitor&) = delete;

    bool is_cs2_focused();
    bool backend_available() const;
    bool has_native_focus_state() const;

    static bool matches_cs2_identity(const std::string& resource_name,
                                     const std::string& resource_class,
                                     const std::string& title,
                                     const std::string& executable = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
