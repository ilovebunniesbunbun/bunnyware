#pragma once

#include <string>
#include <string_view>
#include <vector>

struct MenuSearchEntry {
    std::string label;
    std::string location;
    std::string search_text;
    int tab_index;
    std::string section_id;
    std::string setting_name = {};
};

struct MenuSearchMatch {
    const MenuSearchEntry* entry;
    int score;
};

int fuzzy_match_score(std::string_view query, std::string_view candidate);
const std::vector<MenuSearchEntry>& get_menu_search_entries();
std::vector<MenuSearchMatch> find_menu_search_matches(std::string_view query);
