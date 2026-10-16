#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <uv.h>

#include "channel.h"
#include "co.h"
#include "fs.h"
#include "lunet.h"
#include "lunet_signal.h"
#include "psql.h"
#include "rt.h"
#include "socket.h"
#include "thread.h"
#include "timer.h"

int lunet_open_channel(lua_State *L) {
  luaL_Reg funcs[] = {
    {"create", lunet_channel_create},
    {"close",  lunet_channel_close},
    {"clear",  lunet_channel_clear},
    {"push",   lunet_channel_push},
    {"pop",    lunet_channel_pop},
    {"demand", lunet_channel_demand},
    {"supply", lunet_channel_supply},
    {NULL, NULL}
  };
  luaL_newlib(L, funcs);
  return 1;
}

int lunet_open_thread(lua_State *L) {
  luaL_Reg funcs[] = {
    {"create", lunet_thread_create},
    {"close",  lunet_thread_close},
    {"wait",   lunet_thread_wait},
    {NULL, NULL}
  };
  luaL_newlib(L, funcs);
  return 1;
}

int lunet_open_core(lua_State *L) {
  luaL_Reg funcs[] = {{"spawn", lunet_spawn}, {"sleep", lunet_sleep}, {NULL, NULL}};
  luaL_newlib(L, funcs);
  return 1;
}

int lunet_open_socket(lua_State *L) {
  luaL_Reg funcs[] = {{"listen", lunet_socket_listen},
                      {"accept", lunet_socket_accept},
                      {"getpeername", lunet_socket_getpeername},
                      {"close", lunet_socket_close},
                      {"read", lunet_socket_read},
                      {"write", lunet_socket_write},
                      {"connect", lunet_socket_connect},
                      {"set_read_buffer_size", lunet_socket_set_read_buffer_size},
                      {NULL, NULL}};
  luaL_newlib(L, funcs);
  return 1;
}

int lunet_open_signal(lua_State *L) {
  luaL_Reg funcs[] = {{"wait", lunet_signal_wait}, {NULL, NULL}};
  luaL_newlib(L, funcs);
  return 1;
}

int lunet_open_fs(lua_State *L) {
  luaL_Reg funcs[] = {{"open", lunet_fs_open},
                      {"close", lunet_fs_close},
                      {"read", lunet_fs_read},
                      {"write", lunet_fs_write},
                      {"stat", lunet_fs_stat},
                      {"scandir", lunet_fs_scandir},
                      {NULL, NULL}};
  luaL_newlib(L, funcs);
  return 1;
}

static void register_module(lua_State *L, const char *name, lua_CFunction fn) {
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "preload");
  lua_pushcfunction(L, fn);
  lua_setfield(L, -2, name);
  lua_pop(L, 2);
}

void lunet_open(lua_State *L) {
  register_module(L, "lunet",         lunet_open_core);
  register_module(L, "lunet.socket",  lunet_open_socket);
  register_module(L, "lunet.signal",  lunet_open_signal);
  register_module(L, "lunet.fs",      lunet_open_fs);
  register_module(L, "lunet.channel", lunet_open_channel);
  register_module(L, "lunet.thread",  lunet_open_thread);
  register_module(L, "lunet.psql",    lunet_open_psql);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <lua_file>\n", argv[0]);
    return 1;
  }

  lua_State *L = luaL_newstate();
  if (!L) {
    fprintf(stderr, "failed to create lua_State\n");
    return 1;
  }
  luaL_openlibs(L);

  uv_loop_t *loop = uv_default_loop();
  lunet_rt_t *rt = lunet_rt_create(L, loop);
  if (!rt) {
    fprintf(stderr, "failed to create runtime\n");
    lua_close(L);
    return 1;
  }
  lunet_rt_bind(rt);
  lunet_open(L);

  int status = 0;
  if (luaL_dofile(L, argv[1]) != LUA_OK) {
    const char *error = lua_tostring(L, -1);
    fprintf(stderr, "Error: %s\n", error ? error : "(non-string error)");
    lua_pop(L, 1);
    status = 1;
  } else {
    uv_run(loop, UV_RUN_DEFAULT);
  }

  lunet_rt_destroy(rt);
  lua_close(L);
  return status;
}