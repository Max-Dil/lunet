#include "co.h"

#include <lauxlib.h>
#include <stdio.h>

int lunet_resume(lua_State *co, int nargs, int *nres) {
#if LUA_VERSION_NUM >= 504
  int n = 0;
  int st = lua_resume(co, NULL, nargs, &n);
  if (nres) *nres = n;
  return st;
#else
  if (nres) *nres = 0;
  return lua_resume(co, nargs);
#endif
}

int lunet_ensure_coroutine(lua_State *L, const char *func_name) {
  if (lua_pushthread(L)) {
    lua_pop(L, 1);
    return luaL_error(L, "%s must be called from coroutine", func_name);
  }
  lua_pop(L, 1);
  return 0;
}

int lunet_co_ref(lua_State *co, lunet_rt_t *rt) {
  lua_State *L = lunet_rt_L(rt);
  lua_pushthread(co);
  lua_xmove(co, L, 1);
  return luaL_ref(L, LUA_REGISTRYINDEX);
}

lua_State *lunet_co_fetch(lua_State *L, int ref) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  luaL_unref(L, LUA_REGISTRYINDEX, ref);
  lua_State *co = lua_tothread(L, -1);
  if (!co) lua_pop(L, 1);
  return co;
}

void lunet_co_resume(lua_State *L, lua_State *co, int nargs) {
  int st = lunet_resume(co, nargs, NULL);
  if (st != LUA_OK && st != LUA_YIELD) {
    const char *err = lua_tostring(co, -1);
    fprintf(stderr, "[lunet] coroutine error: %s\n",
            err ? err : "(non-string error)");
  }
  lua_pop(L, 1);
}

int lunet_spawn(lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);

  lua_State *co = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, co, 1);

  int st = lunet_resume(co, 0, NULL);
  if (st != LUA_OK && st != LUA_YIELD) {
    const char *err = lua_tostring(co, -1);
    fprintf(stderr, "[lunet] spawn error: %s\n",
            err ? err : "(non-string error)");
  }
  lua_pop(L, 1);
  return 0;
}