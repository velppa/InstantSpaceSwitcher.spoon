// Lua 5.4 module wrapping ISS (InstantSpaceSwitcher) C library
#include <lua.h>
#include <lauxlib.h>
#include "ISS.h"

static int l_switch_to_index(lua_State *L) {
    unsigned int index = (unsigned int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, iss_switch_to_index(index));
    return 1;
}

static int l_switch_to_index_instant(lua_State *L) {
    unsigned int index = (unsigned int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, iss_switch_to_index_instant(index));
    return 1;
}

static int l_switch_to_index_aug(lua_State *L) {
    unsigned int index = (unsigned int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, iss_switch_to_index_aug(index));
    return 1;
}

static int l_focus_window(lua_State *L) {
    unsigned int wid = (unsigned int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, iss_focus_window(wid));
    return 1;
}

static int l_reset_predictions(lua_State *L) {
    (void)L;
    iss_reset_predictions();
    return 0;
}

static int l_gestures_in_flight(lua_State *L) {
    lua_pushboolean(L, iss_gestures_in_flight());
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
    {"switchToIndex", l_switch_to_index},
    {"switchToIndexInstant", l_switch_to_index_instant},
    {"switchToIndexAug", l_switch_to_index_aug},
    {"getSpaceInfo", l_get_space_info},
    {"focusWindow", l_focus_window},
    {"resetPredictions", l_reset_predictions},
    {"gesturesInFlight", l_gestures_in_flight},
    {NULL, NULL}
};

int luaopen_iss(lua_State *L) {
    luaL_newlib(L, isslib);
    return 1;
}
