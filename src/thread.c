#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "channel.h"
#include "lunet.h"
#include "rt.h"
#include "thread.h"

#define LUNET_THREAD_MT "lunet.thread"

typedef struct thread_s {
  uv_thread_t handle;
  char *dump;
  size_t dump_len;
  int nargs;
  msg_t **args;
  uv_mutex_t mutex;
  uv_cond_t cond;
  int finished;   
  int orphaned;   
  char *error;    
} thread_t;

typedef struct {
  thread_t *th;   
} thread_handle_t;

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  int failed;
} dump_ctx_t;

static char *copy_str(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static const char *error_text(lua_State *L) {
  const char *s = lua_tostring(L, -1);
  return s ? s : "(non-string error)";
}

static void free_args(msg_t **args, int n) {
  if (!args) return;
  for (int i = 0; i < n; i++) lunet_msg_free(args[i]);
  free(args);
}

static void thread_destroy(thread_t *th) {
  if (!th) return;
  free_args(th->args, th->nargs);
  free(th->dump);
  free(th->error);
  uv_mutex_destroy(&th->mutex);
  uv_cond_destroy(&th->cond);
  free(th);
}

static void thread_set_error(thread_t *th, const char *msg) {
  if (th->error || !msg) return;
  th->error = copy_str(msg);
}

static void thread_entry(void *arg) {
  thread_t *th = (thread_t *)arg;
  uv_loop_t *loop = NULL;
  lua_State *L = NULL;
  lunet_rt_t *rt = NULL;

  loop = malloc(sizeof(*loop));
  if (!loop || uv_loop_init(loop) != 0) {
    thread_set_error(th, "failed to create event loop");
    free(loop);
    loop = NULL;
    goto finish;
  }

  L = luaL_newstate();
  if (!L) {
    thread_set_error(th, "failed to create lua_State");
    goto close_loop;
  }
  luaL_openlibs(L);

  rt = lunet_rt_create(L, loop);
  if (!rt) {
    thread_set_error(th, "failed to create runtime");
    goto close_state;
  }
  lunet_rt_bind(rt);
  lunet_open(L);

  if (luaL_loadbuffer(L, th->dump, th->dump_len, "=thread") != LUA_OK) {
    thread_set_error(th, error_text(L));
  } else {
    for (int i = 0; i < th->nargs; i++) lunet_msg_to_lua(L, th->args[i]);
    if (lua_pcall(L, th->nargs, 0, 0) != LUA_OK) {
      thread_set_error(th, error_text(L));
    }
  }

  
  uv_run(loop, UV_RUN_DEFAULT);

  lunet_rt_destroy(rt);
  rt = NULL;

close_state:
  lua_close(L);
  L = NULL;

close_loop:
  if (loop) {
    if (uv_loop_close(loop) != 0) {
      thread_set_error(th, "event loop closed with active handles");
    }
    free(loop);
    loop = NULL;
  }

finish:
  uv_mutex_lock(&th->mutex);
  th->finished = 1;
  int orphaned = th->orphaned;
  uv_cond_broadcast(&th->cond);
  uv_mutex_unlock(&th->mutex);

  if (orphaned) thread_destroy(th);  
}



static int dump_writer(lua_State *L, const void *p, size_t sz, void *ud) {
  (void)L;
  dump_ctx_t *d = (dump_ctx_t *)ud;
  if (d->failed) return 1;
  if (d->len + sz > d->cap) {
    size_t newcap = d->cap ? d->cap : 256;
    while (newcap < d->len + sz) newcap *= 2;
    char *nb = realloc(d->buf, newcap);
    if (!nb) {
      d->failed = 1;
      return 1;
    }
    d->buf = nb;
    d->cap = newcap;
  }
  memcpy(d->buf + d->len, p, sz);
  d->len += sz;
  return 0;
}

static int dump_function(lua_State *L, dump_ctx_t *dc) {
#if LUA_VERSION_NUM >= 504
  return lua_dump(L, dump_writer, dc, 0);
#else
  return lua_dump(L, dump_writer, dc);
#endif
}



static int thread_join(lua_State *L, thread_handle_t *h) {
  thread_t *th = h->th;
  if (!th) {
    lua_pushnil(L);  
    return 1;
  }

  uv_mutex_lock(&th->mutex);
  while (!th->finished) uv_cond_wait(&th->cond, &th->mutex);
  uv_mutex_unlock(&th->mutex);

  uv_thread_join(&th->handle);
  h->th = NULL;

  if (th->error) lua_pushstring(L, th->error);
  else lua_pushnil(L);

  thread_destroy(th);
  return 1;
}

static int thread_gc(lua_State *L) {
  thread_handle_t *h = (thread_handle_t *)luaL_checkudata(L, 1, LUNET_THREAD_MT);
  thread_t *th = h->th;
  if (!th) return 0;
  h->th = NULL;

  uv_mutex_lock(&th->mutex);
  if (th->finished) {
    uv_mutex_unlock(&th->mutex);
    uv_thread_join(&th->handle);
    thread_destroy(th);
  } else {
    th->orphaned = 1;  
    uv_mutex_unlock(&th->mutex);
  }
  return 0;
}

static void register_handle_mt(lua_State *L) {
  if (luaL_newmetatable(L, LUNET_THREAD_MT)) {
    lua_pushcfunction(L, thread_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_pop(L, 1);
}



int lunet_thread_create(lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);

  dump_ctx_t dc = {0};
  lua_pushvalue(L, 1);
  int rc = dump_function(L, &dc);
  lua_pop(L, 1);
  if (rc != 0 || dc.failed) {
    free(dc.buf);
    lua_pushnil(L);
    lua_pushstring(L, "failed to dump function (upvalues not supported)");
    return 2;
  }

  int nargs = lua_gettop(L) - 1;
  msg_t **args = NULL;
  if (nargs > 0) {
    args = calloc((size_t)nargs, sizeof(msg_t *));
    if (!args) {
      free(dc.buf);
      lua_pushnil(L);
      lua_pushstring(L, "out of memory");
      return 2;
    }
    for (int i = 0; i < nargs; i++) {
      args[i] = lunet_msg_from_lua(L, i + 2);
      if (!args[i]) {
        free_args(args, i);
        free(dc.buf);
        lua_pushnil(L);
        lua_pushstring(L, "unsupported argument type");
        return 2;
      }
    }
  }

  thread_t *th = calloc(1, sizeof(*th));
  if (!th) {
    free_args(args, nargs);
    free(dc.buf);
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  th->dump = dc.buf;
  th->dump_len = dc.len;
  th->nargs = nargs;
  th->args = args;
  uv_mutex_init(&th->mutex);
  uv_cond_init(&th->cond);

  rc = uv_thread_create(&th->handle, thread_entry, th);
  if (rc != 0) {
    thread_destroy(th);
    lua_pushnil(L);
    lua_pushfstring(L, "failed to create thread: %s", uv_strerror(rc));
    return 2;
  }

  register_handle_mt(L);
  thread_handle_t *h = (thread_handle_t *)lua_newuserdata(L, sizeof(*h));
  h->th = th;
  luaL_setmetatable(L, LUNET_THREAD_MT);
  return 1;
}



int lunet_thread_wait(lua_State *L) {
  thread_handle_t *h = (thread_handle_t *)luaL_checkudata(L, 1, LUNET_THREAD_MT);
  return thread_join(L, h);
}

int lunet_thread_close(lua_State *L) {
  thread_handle_t *h = (thread_handle_t *)luaL_checkudata(L, 1, LUNET_THREAD_MT);
  return thread_join(L, h);
}