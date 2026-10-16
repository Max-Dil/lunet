#ifndef LUNET_LUA_COMPAT_H
#define LUNET_LUA_COMPAT_H

#include <stddef.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#if LUA_VERSION_NUM < 502

#ifndef luaL_newlib
#define luaL_newlib(L, l) \
  (lua_createtable((L), 0, sizeof(l) / sizeof((l)[0]) - 1), \
   luaL_register((L), NULL, (l)))
#endif

#ifndef luaL_setmetatable
#define luaL_setmetatable(L, tname) \
  (luaL_getmetatable((L), (tname)), lua_setmetatable((L), -2))
#endif

#ifndef LUA_MAXINTEGER
#define LUA_MAXINTEGER ((lua_Integer)(((size_t)-1) >> 1))
#endif

#ifndef LUA_MININTEGER
#define LUA_MININTEGER (-LUA_MAXINTEGER - 1)
#endif

#endif

#endif