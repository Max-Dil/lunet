#ifndef LUNET_THREAD_H
#define LUNET_THREAD_H

#include <lua.h>

int lunet_thread_create(lua_State *L);
int lunet_thread_close(lua_State *L);
int lunet_thread_wait(lua_State *L);

#endif