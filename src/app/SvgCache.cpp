#include "SvgCache.hpp"
#include "logger.hpp"
#include "config.hpp"
#include "overlay/game_reader.hpp"
#include <GL/glew.h>

#define NANOSVG_IMPLEMENTATION
#include "external/nanosvg/nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "external/nanosvg/nanosvgrast.h"

SvgCache::SvgCache() {}

SvgCache::~SvgCache() {
    for (auto& pair : textures) {
        if (pair.second.id != 0) {
            glDeleteTextures(1, &pair.second.id);
        }
    }
    for (auto& pair : weapon_textures) {
        if (pair.second.id != 0) {
            glDeleteTextures(1, &pair.second.id);
        }
    }
}

#include <filesystem>

void SvgCache::initialize(const std::string& assets_dir) {
    std::string actual_dir = assets_dir;
    if (!std::filesystem::exists(actual_dir)) {
        if (std::filesystem::exists("../" + assets_dir)) {
            actual_dir = "../" + assets_dir;
        }
    }

    assets_path = actual_dir;

    const std::string ingame_icons_dir = actual_dir + "/icons/ingame";
    BUNNYWARE_LOG_INFO("Loading SVG assets from {}", ingame_icons_dir);
    textures[GRENADE_HE]      = load_svg_to_texture(ingame_icons_dir + "/hegrenade.svg");
    textures[GRENADE_FLASH]   = load_svg_to_texture(ingame_icons_dir + "/flashbang.svg");
    textures[GRENADE_SMOKE]   = load_svg_to_texture(ingame_icons_dir + "/smokegrenade.svg");
    textures[GRENADE_MOLOTOV] = load_svg_to_texture(ingame_icons_dir + "/molotov.svg");
    textures[GRENADE_DECOY]   = load_svg_to_texture(ingame_icons_dir + "/decoy.svg");
    textures[GRENADE_C4]      = load_svg_to_texture(ingame_icons_dir + "/c4.svg");
}

SvgTexture SvgCache::get_texture(uint8_t grenade_type) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = textures.find(grenade_type);
    if (it != textures.end()) {
        return it->second;
    }
    return SvgTexture{0, 0, 0};
}

SvgTexture SvgCache::get_weapon_texture(const std::string& weapon_name) const {
    if (weapon_name.empty()) return SvgTexture{0, 0, 0};
    
    std::lock_guard<std::mutex> lock(mutex);
    auto it = weapon_textures.find(weapon_name);
    if (it != weapon_textures.end()) {
        return it->second;
    }

    const bool explicit_asset_path = weapon_name.find('/') != std::string::npos;
    std::string path = assets_path + "/"
        + (explicit_asset_path ? weapon_name : "icons/ingame/" + weapon_name)
        + ".svg";
    if (std::filesystem::exists(path)) {
        SvgTexture tex = const_cast<SvgCache*>(this)->load_svg_to_texture(path);
        weapon_textures[weapon_name] = tex;
        return tex;
    }
    return SvgTexture{0, 0, 0};
}

SvgTexture SvgCache::load_svg_to_texture(const std::string& path) {
    NSVGimage* image = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (image == nullptr) {
        BUNNYWARE_LOG_ERROR("Could not load SVG from {}", path);
        return SvgTexture{0, 0, 0};
    }

    int w = (int)image->width;
    int h = (int)image->height;

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (rast == nullptr) {
        BUNNYWARE_LOG_ERROR("Could not initialize SVG rasterizer");
        nsvgDelete(image);
        return SvgTexture{0, 0, 0};
    }

    unsigned char* img = (unsigned char*)malloc(w * h * 4);
    if (img == nullptr) {
        BUNNYWARE_LOG_ERROR("Could not allocate memory for SVG rasterization");
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return SvgTexture{0, 0, 0};
    }

    nsvgRasterize(rast, image, 0, 0, 1.0f, img, w, h, w * 4);

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    
    // Set texture wrapping to GL_CLAMP_TO_EDGE
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);	
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Set texture interpolation
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);

    free(img);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    return SvgTexture{tex_id, (float)w, (float)h};
}
