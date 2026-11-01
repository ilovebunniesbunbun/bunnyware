#pragma once
#include <spdlog/spdlog.h>

#define BUNNYWARE_LOG_INFO(...)  spdlog::info("[BUNNYWARE Chams] " __VA_ARGS__)
#define BUNNYWARE_LOG_WARN(...)  spdlog::warn("[BUNNYWARE Chams] " __VA_ARGS__)
#define BUNNYWARE_LOG_ERROR(...) spdlog::error("[BUNNYWARE Chams] " __VA_ARGS__)
#define BUNNYWARE_LOG_DEBUG(...) spdlog::debug("[BUNNYWARE Chams] " __VA_ARGS__)
