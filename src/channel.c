#include "channel.h"

#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "co.h"
#include "rt.h"



msg_t *lunet_msg_from_lua(lua_State *L, int idx) {
  msg_t *m = calloc(1, sizeof(*m));
  if (!m) return NULL;

  switch (lua_type(L, idx)) {
    case LUA_TNIL:
      m->type = MSG_NIL;
      break;
    case LUA_TBOOLEAN:
      m->type = MSG_BOOL;
      m->v.b = lua_toboolean(L, idx);
      break;
    case LUA_TNUMBER: {
      lua_Number n = lua_tonumber(L, idx);
      if (n >= (lua_Number)LUA_MININTEGER && n <= (lua_Number)LUA_MAXINTEGER &&
          (lua_Number)(lua_Integer)n == n) {
        m->type = MSG_INTEGER;
        m->v.i = (lua_Integer)n;
      } else {
        m->type = MSG_NUMBER;
        m->v.n = n;
      }
      break;
    }
    case LUA_TSTRING: {
      size_t len = 0;
      const char *s = lua_tolstring(L, idx, &len);
      m->type = MSG_STRING;
      m->v.s.data = malloc(len ? len : 1);
      if (!m->v.s.data) {
        free(m);
        return NULL;
      }
      memcpy(m->v.s.data, s, len);
      m->v.s.len = len;
      break;
    }
    default:
      free(m);
      return NULL;
  }
  return m;
}

void lunet_msg_to_lua(lua_State *L, const msg_t *m) {
  switch (m->type) {
    case MSG_NIL:     lua_pushnil(L); break;
    case MSG_BOOL:    lua_pushboolean(L, m->v.b); break;
    case MSG_INTEGER: lua_pushinteger(L, m->v.i); break;
    case MSG_NUMBER:  lua_pushnumber(L, m->v.n); break;
    case MSG_STRING:  lua_pushlstring(L, m->v.s.data, m->v.s.len); break;
  }
}

void lunet_msg_free(msg_t *m) {
  if (!m) return;
  if (m->type == MSG_STRING && m->v.s.data) free(m->v.s.data);
  free(m);
}



typedef struct waiter_s {
  lunet_rt_t *rt;        
  int co_ref;            
  struct message_s *msg; 
  const char *err;       
  struct waiter_s *next;
} waiter_t;

typedef struct message_s {
  msg_t msg;
  waiter_t *ack;         
  struct message_s *next;
} message_t;

typedef struct channel_s {
  char *name;
  uv_mutex_t mutex;
  int closed;
  int registered;
  message_t *mhead, *mtail;   
  waiter_t *whead, *wtail;    
  struct channel_s *reg_next;
} channel_t;


static uv_once_t g_reg_once = UV_ONCE_INIT;
static uv_mutex_t g_reg_mutex;
static channel_t *g_registry = NULL;

static void reg_init(void) { uv_mutex_init(&g_reg_mutex); }

static char *copy_str(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static message_t *message_new(lua_State *L, int idx) {
  msg_t *m = lunet_msg_from_lua(L, idx);
  if (!m) return NULL;
  message_t *node = calloc(1, sizeof(*node));
  if (!node) {
    lunet_msg_free(m);
    return NULL;
  }
  node->msg = *m;
  free(m);
  return node;
}

static void message_destroy(message_t *m) {
  if (!m) return;
  if (m->msg.type == MSG_STRING && m->msg.v.s.data) free(m->msg.v.s.data);
  free(m);
}

static waiter_t *waiter_new(lunet_rt_t *rt, lua_State *co) {
  waiter_t *w = calloc(1, sizeof(*w));
  if (!w) return NULL;
  w->rt = rt;
  w->co_ref = lunet_co_ref(co, rt);
  return w;
}


static void waiter_discard(waiter_t *w) {
  luaL_unref(lunet_rt_L(w->rt), LUA_REGISTRYINDEX, w->co_ref);
  free(w);
}



static void resume_ack(lunet_rt_t *rt, void *arg) {
  waiter_t *w = (waiter_t *)arg;
  lua_State *L = lunet_rt_L(rt);
  lua_State *co = lunet_co_fetch(L, w->co_ref);
  if (co) {
    if (w->err) lua_pushstring(co, w->err);
    else lua_pushnil(co);
    lunet_co_resume(L, co, 1);
  }
  lunet_rt_wait_end(rt);
  free(w);
}

static void resume_recv(lunet_rt_t *rt, void *arg) {
  waiter_t *w = (waiter_t *)arg;
  lua_State *L = lunet_rt_L(rt);
  lua_State *co = lunet_co_fetch(L, w->co_ref);
  if (co) {
    if (w->msg) {
      lunet_msg_to_lua(co, &w->msg->msg);
      lua_pushnil(co);
    } else {
      lua_pushnil(co);
      lua_pushstring(co, "channel closed");
    }
    lunet_co_resume(L, co, 2);
  }
  if (w->msg) {
    message_t *m = w->msg;
    w->msg = NULL;
    
    
    if (m->ack) {
      waiter_t *a = m->ack;
      m->ack = NULL;
      a->err = NULL;
      if (lunet_rt_post(a->rt, resume_ack, a) != 0) {
        fprintf(stderr, "[lunet] channel: failed to wake supplier\n");
      }
    }
    message_destroy(m);
  }
  lunet_rt_wait_end(rt);
  free(w);
}



static void post_recv(waiter_t *w) {
  if (lunet_rt_post(w->rt, resume_recv, w) != 0) {
    fprintf(stderr, "[lunet] channel: failed to wake receiver\n");
  }
}


static void message_ack(message_t *m, const char *err) {
  waiter_t *w = m->ack;
  if (!w) return;
  m->ack = NULL;
  w->err = err;
  if (lunet_rt_post(w->rt, resume_ack, w) != 0) {
    fprintf(stderr, "[lunet] channel: failed to wake supplier\n");
  }
}



static void mq_push(channel_t *ch, message_t *m) {
  m->next = NULL;
  if (ch->mtail) ch->mtail->next = m;
  else ch->mhead = m;
  ch->mtail = m;
}

static message_t *mq_pop(channel_t *ch) {
  message_t *m = ch->mhead;
  if (m) {
    ch->mhead = m->next;
    if (!ch->mhead) ch->mtail = NULL;
  }
  return m;
}

static void wq_push(channel_t *ch, waiter_t *w) {
  w->next = NULL;
  if (ch->wtail) ch->wtail->next = w;
  else ch->whead = w;
  ch->wtail = w;
}

static waiter_t *wq_pop(channel_t *ch) {
  waiter_t *w = ch->whead;
  if (w) {
    ch->whead = w->next;
    if (!ch->whead) ch->wtail = NULL;
  }
  return w;
}

static void drain_messages(channel_t *ch, const char *err) {
  message_t *m;
  while ((m = mq_pop(ch)) != NULL) {
    message_ack(m, err);
    message_destroy(m);
  }
}

static channel_t *channel_arg(lua_State *L, int idx) {
  return (channel_t *)lua_touserdata(L, idx);
}

static channel_t *channel_new(const char *name) {
  channel_t *ch = calloc(1, sizeof(*ch));
  if (!ch) return NULL;
  ch->name = copy_str(name);
  if (!ch->name || uv_mutex_init(&ch->mutex) != 0) {
    free(ch->name);
    free(ch);
    return NULL;
  }
  return ch;
}

static void unregister(channel_t *ch) {
  uv_mutex_lock(&g_reg_mutex);
  if (ch->registered) {
    channel_t **pp = &g_registry;
    while (*pp) {
      if (*pp == ch) {
        *pp = ch->reg_next;
        break;
      }
      pp = &(*pp)->reg_next;
    }
    ch->registered = 0;
    ch->reg_next = NULL;
  }
  uv_mutex_unlock(&g_reg_mutex);
}



int lunet_channel_create(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  uv_once(&g_reg_once, reg_init);

  uv_mutex_lock(&g_reg_mutex);
  channel_t *ch = NULL;
  for (channel_t *c = g_registry; c; c = c->reg_next) {
    if (strcmp(c->name, name) == 0) {
      ch = c;
      break;
    }
  }
  if (!ch) {
    ch = channel_new(name);
    if (ch) {
      ch->registered = 1;
      ch->reg_next = g_registry;
      g_registry = ch;
    }
  }
  uv_mutex_unlock(&g_reg_mutex);

  if (!ch) {
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  lua_pushlightuserdata(L, ch);
  return 1;
}

int lunet_channel_close(lua_State *L) {
  channel_t *ch = channel_arg(L, 1);
  if (!ch) {
    lua_pushstring(L, "invalid channel handle");
    return 1;
  }

  uv_mutex_lock(&ch->mutex);
  if (ch->closed) {
    uv_mutex_unlock(&ch->mutex);
    lua_pushstring(L, "channel already closed");
    return 1;
  }
  ch->closed = 1;
  drain_messages(ch, "channel closed");
  waiter_t *w = ch->whead;  
  ch->whead = ch->wtail = NULL;
  uv_mutex_unlock(&ch->mutex);

  while (w) {
    waiter_t *next = w->next;
    w->msg = NULL;  
    post_recv(w);
    w = next;
  }

  unregister(ch);
  lua_pushnil(L);
  return 1;
}

int lunet_channel_clear(lua_State *L) {
  channel_t *ch = channel_arg(L, 1);
  if (!ch) {
    lua_pushstring(L, "invalid channel handle");
    return 1;
  }

  uv_mutex_lock(&ch->mutex);
  drain_messages(ch, "channel cleared");
  uv_mutex_unlock(&ch->mutex);

  lua_pushnil(L);
  return 1;
}

int lunet_channel_push(lua_State *L) {
  channel_t *ch = channel_arg(L, 1);
  if (!ch) {
    lua_pushstring(L, "invalid channel handle");
    return 1;
  }

  message_t *m = message_new(L, 2);
  if (!m) {
    lua_pushstring(L, "unsupported value or out of memory");
    return 1;
  }

  uv_mutex_lock(&ch->mutex);
  if (ch->closed) {
    uv_mutex_unlock(&ch->mutex);
    message_destroy(m);
    lua_pushstring(L, "channel closed");
    return 1;
  }
  waiter_t *w = wq_pop(ch);
  if (w) {
    w->msg = m;  
  } else {
    mq_push(ch, m);
  }
  uv_mutex_unlock(&ch->mutex);

  if (w) post_recv(w);

  lua_pushnil(L);
  return 1;
}

int lunet_channel_pop(lua_State *L) {
  channel_t *ch = channel_arg(L, 1);
  if (!ch) {
    lua_pushnil(L);
    lua_pushstring(L, "invalid channel handle");
    return 2;
  }

  uv_mutex_lock(&ch->mutex);
  message_t *m = mq_pop(ch);
  int closed = ch->closed;
  uv_mutex_unlock(&ch->mutex);

  if (m) {
    lunet_msg_to_lua(L, &m->msg);
    lua_pushnil(L);
    message_ack(m, NULL);
    message_destroy(m);
    return 2;
  }

  lua_pushnil(L);
  lua_pushstring(L, closed ? "channel closed" : "channel empty");
  return 2;
}

int lunet_channel_demand(lua_State *L) {
  channel_t *ch = channel_arg(L, 1);
  if (!ch) {
    lua_pushnil(L);
    lua_pushstring(L, "invalid channel handle");
    return 2;
  }
  if (lunet_ensure_coroutine(L, "channel.demand") != 0) return 0;

  lunet_rt_t *rt = lunet_rt_current();
  if (!rt) return luaL_error(L, "channel.demand: no runtime in this thread");

  
  waiter_t *w = waiter_new(rt, L);
  if (!w) {
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }

  uv_mutex_lock(&ch->mutex);
  message_t *m = mq_pop(ch);
  if (!m && ch->closed) {
    uv_mutex_unlock(&ch->mutex);
    waiter_discard(w);
    lua_pushnil(L);
    lua_pushstring(L, "channel closed");
    return 2;
  }
  if (!m) {
    wq_push(ch, w);
    lunet_rt_wait_begin(rt);
    uv_mutex_unlock(&ch->mutex);
    return lua_yield(L, 0);  
  }
  uv_mutex_unlock(&ch->mutex);

  waiter_discard(w);  
  lunet_msg_to_lua(L, &m->msg);
  lua_pushnil(L);
  message_ack(m, NULL);
  message_destroy(m);
  return 2;
}

int lunet_channel_supply(lua_State *L) {
  channel_t *ch = channel_arg(L, 1);
  if (!ch) {
    lua_pushstring(L, "invalid channel handle");
    return 1;
  }
  if (lunet_ensure_coroutine(L, "channel.supply") != 0) return 0;

  lunet_rt_t *rt = lunet_rt_current();
  if (!rt) return luaL_error(L, "channel.supply: no runtime in this thread");

  message_t *m = message_new(L, 2);
  if (!m) {
    lua_pushstring(L, "unsupported value or out of memory");
    return 1;
  }
  waiter_t *aw = waiter_new(rt, L);
  if (!aw) {
    message_destroy(m);
    lua_pushstring(L, "out of memory");
    return 1;
  }
  m->ack = aw;

  uv_mutex_lock(&ch->mutex);
  if (ch->closed) {
    uv_mutex_unlock(&ch->mutex);
    m->ack = NULL;
    waiter_discard(aw);
    message_destroy(m);
    lua_pushstring(L, "channel closed");
    return 1;
  }
  waiter_t *w = wq_pop(ch);
  if (w) {
    w->msg = m;
  } else {
    mq_push(ch, m);
  }
  lunet_rt_wait_begin(rt);  
  uv_mutex_unlock(&ch->mutex);

  if (w) post_recv(w);

  return lua_yield(L, 0);  
}