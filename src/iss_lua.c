// Lua 5.4 module wrapping ISS (InstantSpaceSwitcher) C library
#include <lua.h>
#include <lauxlib.h>
#include <stdbool.h>

// ISS declarations
extern bool iss_init(void);
extern void iss_destroy(void);
extern bool iss_switch_to_index(unsigned int targetIndex);

typedef struct {
    unsigned int currentIndex;
    unsigned int spaceCount;
} ISSSpaceInfo;
extern bool iss_get_menubar_space_info(ISSSpaceInfo *info);

static int l_init(lua_State *L) {
    lua_pushboolean(L, iss_init());
    return 1;
}

static int l_destroy(lua_State *L) {
    iss_destroy();
    return 0;
}

static int l_switch_to_index(lua_State *L) {
    unsigned int index = (unsigned int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, iss_switch_to_index(index));
    return 1;
}

static int l_get_space_info(lua_State *L) {
    ISSSpaceInfo info;
    if (iss_get_menubar_space_info(&info)) {
        lua_newtable(L);
        lua_pushinteger(L, info.currentIndex + 1);
        lua_setfield(L, -2, "current");
        lua_pushinteger(L, info.spaceCount);
        lua_setfield(L, -2, "count");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static const struct luaL_Reg isslib[] = {
    {"init", l_init},
    {"destroy", l_destroy},
    {"switchToIndex", l_switch_to_index},
    {"getSpaceInfo", l_get_space_info},
    {NULL, NULL}
};

int luaopen_iss(lua_State *L) {
    luaL_newlib(L, isslib);
    return 1;
}
