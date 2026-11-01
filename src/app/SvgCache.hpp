#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <mutex>

typedef unsigned int GLuint;

struct SvgTexture {
    GLuint id = 0;
    float width = 0;
    float height = 0;
};

class SvgCache {
public:
    SvgCache();
    ~SvgCache();

    void initialize(const std::string& assets_dir);
    SvgTexture get_texture(uint8_t grenade_type) const;
    SvgTexture get_weapon_texture(const std::string& weapon_name) const;

private:
    SvgTexture load_svg_to_texture(const std::string& path);
    std::string assets_path;
    mutable std::mutex mutex;
    std::unordered_map<uint8_t, SvgTexture> textures;
    mutable std::unordered_map<std::string, SvgTexture> weapon_textures;
};
