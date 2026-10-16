#ifndef LUNET_CO_H
#define LUNET_CO_H

#include <lua.h>
#include "rt.h"

int lunet_spawn(lua_State *L);
int lunet_ensure_coroutine(lua_State *L, const char *func_name);

int lunet_resume(lua_State *co, int nargs, int *nres);

int lunet_co_ref(lua_State *co, lunet_rt_t *rt);

lua_State *lunet_co_fetch(lua_State *L, int ref);

void lunet_co_resume(lua_State *L, lua_State *co, int nargs);

#endif