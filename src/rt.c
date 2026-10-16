#include "rt.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct task_s {
  lunet_task_fn fn;
  void *arg;
  struct task_s *next;
} task_t;

struct lunet_rt_s {
  lua_State *L;
  uv_loop_t *loop;
  uv_async_t wake;
  uv_mutex_t mutex;
  task_t *head;
  task_t *tail;
  int waiting;
  int wake_closed;
};

static _Thread_local lunet_rt_t *tls_rt = NULL;

static void wake_close_cb(uv_handle_t *h) {
  ((lunet_rt_t *)h->data)->wake_closed = 1;
}

static void wake_cb(uv_async_t *h) {
  lunet_rt_t *rt = (lunet_rt_t *)h->data;
  for (;;) {
    uv_mutex_lock(&rt->mutex);
    task_t *t = rt->head;
    if (t) {
      rt->head = t->next;
      if (!rt->head) rt->tail = NULL;
    }
    uv_mutex_unlock(&rt->mutex);
    if (!t) break;
    t->fn(rt, t->arg);
    free(t);
  }
}

lunet_rt_t *lunet_rt_create(lua_State *L, uv_loop_t *loop) {
  lunet_rt_t *rt = calloc(1, sizeof(*rt));
  if (!rt) return NULL;
  rt->L = L;
  rt->loop = loop;

  if (uv_mutex_init(&rt->mutex) != 0) {
    free(rt);
    return NULL;
  }
  if (uv_async_init(loop, &rt->wake, wake_cb) != 0) {
    uv_mutex_destroy(&rt->mutex);
    free(rt);
    return NULL;
  }
  rt->wake.data = rt;
  uv_unref((uv_handle_t *)&rt->wake);
  return rt;
}

void lunet_rt_destroy(lunet_rt_t *rt) {
  if (!rt) return;

  if (!uv_is_closing((uv_handle_t *)&rt->wake)) {
    uv_close((uv_handle_t *)&rt->wake, wake_close_cb);
  }
  while (!rt->wake_closed) uv_run(rt->loop, UV_RUN_NOWAIT);

  task_t *t = rt->head;
  while (t) {
    task_t *n = t->next;
    free(t);
    t = n;
  }
  uv_mutex_destroy(&rt->mutex);
  if (tls_rt == rt) tls_rt = NULL;
  free(rt);
}

void lunet_rt_bind(lunet_rt_t *rt) { tls_rt = rt; }

lunet_rt_t *lunet_rt_current(void) { return tls_rt; }

lua_State *lunet_rt_L(lunet_rt_t *rt) { return rt->L; }

uv_loop_t *lunet_rt_loop(lunet_rt_t *rt) { return rt->loop; }

int lunet_rt_post(lunet_rt_t *rt, lunet_task_fn fn, void *arg) {
  task_t *t = malloc(sizeof(*t));
  if (!t) return -1;
  t->fn = fn;
  t->arg = arg;
  t->next = NULL;

  uv_mutex_lock(&rt->mutex);
  if (rt->tail) rt->tail->next = t;
  else rt->head = t;
  rt->tail = t;
  uv_mutex_unlock(&rt->mutex);

  return uv_async_send(&rt->wake);
}

void lunet_rt_wait_begin(lunet_rt_t *rt) {
  if (rt->waiting++ == 0) uv_ref((uv_handle_t *)&rt->wake);
}

void lunet_rt_wait_end(lunet_rt_t *rt) {
  if (--rt->waiting == 0) uv_unref((uv_handle_t *)&rt->wake);
}

uv_loop_t *lunet_loop(void) {
  return tls_rt ? tls_rt->loop : uv_default_loop();
}

lua_State *default_luaL(void) {
  return tls_rt ? tls_rt->L : NULL;
}