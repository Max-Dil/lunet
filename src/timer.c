#include "timer.h"

#include <lauxlib.h>
#include <stdlib.h>
#include <uv.h>

#include "co.h"
#include "rt.h"

typedef struct {
  uv_timer_t timer;  
  lunet_rt_t *rt;
  int co_ref;
} sleep_ctx_t;

static void sleep_close_cb(uv_handle_t *h) { free(h); }

static void sleep_cb(uv_timer_t *timer) {
  sleep_ctx_t *ctx = (sleep_ctx_t *)timer;
  lunet_rt_t *rt = ctx->rt;
  int ref = ctx->co_ref;

  uv_close((uv_handle_t *)timer, sleep_close_cb);

  lua_State *L = lunet_rt_L(rt);
  lua_State *co = lunet_co_fetch(L, ref);
  if (co) lunet_co_resume(L, co, 0);
}


int lunet_sleep(lua_State *co) {
  if (lunet_ensure_coroutine(co, "lunet.sleep") != 0) return 0;

  lua_Integer ms = luaL_checkinteger(co, 1);
  if (ms < 0) return luaL_error(co, "lunet.sleep duration must be >= 0");

  lunet_rt_t *rt = lunet_rt_current();
  if (!rt) return luaL_error(co, "lunet.sleep: no runtime in this thread");

  sleep_ctx_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return luaL_error(co, "lunet.sleep: out of memory");

  ctx->rt = rt;
  ctx->co_ref = lunet_co_ref(co, rt);

  uv_timer_init(lunet_rt_loop(rt), &ctx->timer);
  uv_timer_start(&ctx->timer, sleep_cb, (uint64_t)ms, 0);

  return lua_yield(co, 0);
}