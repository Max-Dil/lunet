#ifndef LUNET_RT_H
#define LUNET_RT_H

#include "lua_compat.h"
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lunet_rt_s lunet_rt_t;

typedef void (*lunet_task_fn)(lunet_rt_t *rt, void *arg);

lunet_rt_t *lunet_rt_create(lua_State *L, uv_loop_t *loop);
void lunet_rt_destroy(lunet_rt_t *rt);

void lunet_rt_bind(lunet_rt_t *rt);

lunet_rt_t *lunet_rt_current(void);
lua_State *lunet_rt_L(lunet_rt_t *rt);
uv_loop_t *lunet_rt_loop(lunet_rt_t *rt);

int lunet_rt_post(lunet_rt_t *rt, lunet_task_fn fn, void *arg);

void lunet_rt_wait_begin(lunet_rt_t *rt);
void lunet_rt_wait_end(lunet_rt_t *rt);

uv_loop_t *lunet_loop(void);

lua_State *default_luaL(void);

#ifdef __cplusplus
}
#endif

#endif