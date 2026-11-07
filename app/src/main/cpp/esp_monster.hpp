#pragma once

inline bool esp_monster_id(int id) {
    static const int k[] = {2002, 2003, 2004, 2005, 2006, 2008, 2009, 2011, 2012, 2013,
                            2056, 2059, 2072, 2110, 2220, 2221, 2222, 2223};
    for (int m : k)
        if (m == id) return true;
    return false;
}

inline const char *esp_monster_label(int id) {
    switch (id) {
    case 2002: return "Lord";
    case 2003:
    case 2110: return "Turtle";
    case 2004:
    case 2220: return "Fiend";
    case 2005:
    case 2221: return "Serpent";
    case 2006: return "Lizard";
    case 2008:
    case 2059: return "Crammer";
    case 2009: return "Rockursa";
    case 2011:
    case 2013:
    case 2222:
    case 2223: return "Crab";
    case 2012: return "SerpentK";
    case 2056:
    case 2072: return "Litho";
    default: return "Mob";
    }
}
