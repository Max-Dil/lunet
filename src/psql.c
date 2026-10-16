#include "psql.h"

#include <libpq-fe.h>
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "co.h"
#include "rt.h"

#define PSQL_POOL_MT "lunet.psql.pool"
#define PSQL_CONN_MT "lunet.psql.conn"

#define OID_BOOL 16
#define OID_INT8 20
#define OID_INT2 21
#define OID_INT4 23
#define OID_FLOAT4 700
#define OID_FLOAT8 701
#define PSQL_MAX_SAFE_INT 9007199254740992.0

enum { REQ_QUERY, REQ_EXEC, REQ_ACQUIRE };
enum { CONN_FREE, CONN_CONNECTING, CONN_IDLE, CONN_LEASED };
enum { STAGE_NONE, STAGE_CONNECT, STAGE_PREPARE, STAGE_RUN };

typedef struct pool_s pool_t;
typedef struct conn_s conn_t;
typedef struct req_s req_t;

typedef struct sql_s {
  char *name;
  char *sql;
  struct sql_s *next;
} sql_t;

typedef struct stmt_s {
  sql_t *entry;
  struct stmt_s *next;
} stmt_t;

struct req_s {
  lunet_rt_t *rt;
  int co_ref;
  int kind;
  int release;
  int nparams;
  char **params;
  char *sql;
  char *name;
  req_t *next;
};

struct conn_s {
  pool_t *pool;
  PGconn *pg;
  PGresult *result;
  uv_poll_t poll;
  int poll_init;
  int state;
  int stage;
  int acquired;
  int orphaned;
  unsigned gen;
  req_t *req;
  stmt_t *stmts;
  conn_t *next;
};

struct pool_s {
  lunet_rt_t *rt;
  char **kw;
  char **val;
  int max_conns;
  int count;
  int closed;
  conn_t *idle;
  conn_t *free_list;
  req_t *wait_head;
  req_t *wait_tail;
  sql_t *stmts;
};

typedef struct {
  conn_t *c;
  unsigned gen;
} conn_handle_t;

static void req_finish_error(req_t *r, const char *msg);
static void conn_fail(conn_t *c, const char *msg);
static void conn_release(conn_t *c);
static void conn_assign(conn_t *c, req_t *w);
static void pool_pump(pool_t *p);
static void finish_acquire(conn_t *c, req_t *w);
static void poll_cb(uv_poll_t *h, int status, int events);
static void conn_connect_step(conn_t *c);
static void conn_io_step(conn_t *c);
static int pool_start(lua_State *L, pool_t *p, req_t *r);

static char *dup_str(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static char *dup_len(const char *s, size_t n) {
  char *p = malloc(n + 1);
  if (p) {
    memcpy(p, s, n);
    p[n] = '\0';
  }
  return p;
}

static int push_err(lua_State *L, const char *msg) {
  lua_pushnil(L);
  lua_pushstring(L, msg);
  return 2;
}

static int push_owned_err(lua_State *L, char *msg) {
  int n = push_err(L, msg ? msg : "out of memory");
  free(msg);
  return n;
}

static void push_cell(lua_State *L, PGresult *res, int row, int col) {
  if (PQgetisnull(res, row, col)) {
    lua_pushnil(L);
    return;
  }
  const char *v = PQgetvalue(res, row, col);
  int len = PQgetlength(res, row, col);
  switch (PQftype(res, col)) {
    case OID_BOOL:
      lua_pushboolean(L, v[0] == 't');
      return;
    case OID_INT2:
    case OID_INT4:
      lua_pushnumber(L, (lua_Number)strtol(v, NULL, 10));
      return;
    case OID_INT8: {
      long long x = strtoll(v, NULL, 10);
      if (x >= -(long long)PSQL_MAX_SAFE_INT && x <= (long long)PSQL_MAX_SAFE_INT)
        lua_pushnumber(L, (lua_Number)x);
      else
        lua_pushlstring(L, v, (size_t)len);
      return;
    }
    case OID_FLOAT4:
    case OID_FLOAT8:
      lua_pushnumber(L, strtod(v, NULL));
      return;
    default:
      lua_pushlstring(L, v, (size_t)len);
      return;
  }
}

static int push_result(lua_State *L, PGresult *res) {
  switch (PQresultStatus(res)) {
    case PGRES_TUPLES_OK: {
      int nrows = PQntuples(res);
      int ncols = PQnfields(res);
      lua_createtable(L, nrows, 0);
      for (int i = 0; i < nrows; i++) {
        lua_createtable(L, 0, ncols);
        for (int j = 0; j < ncols; j++) {
          push_cell(L, res, i, j);
          lua_setfield(L, -2, PQfname(res, j));
        }
        lua_rawseti(L, -2, i + 1);
      }
      lua_pushnumber(L, (lua_Number)nrows);
      return 2;
    }
    case PGRES_COMMAND_OK:
    case PGRES_EMPTY_QUERY: {
      const char *t = PQcmdTuples(res);
      lua_createtable(L, 0, 0);
      lua_pushnumber(L, (lua_Number)(t && *t ? strtoll(t, NULL, 10) : 0));
      return 2;
    }
    default: {
      const char *state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
      lua_pushnil(L);
      lua_pushstring(L, PQresultErrorMessage(res));
      if (state) lua_pushstring(L, state);
      else lua_pushnil(L);
      return 3;
    }
  }
}

static sql_t *pool_find_entry(pool_t *p, const char *name) {
  for (sql_t *e = p->stmts; e; e = e->next)
    if (strcmp(e->name, name) == 0) return e;
  return NULL;
}

static int conn_has_stmt(conn_t *c, sql_t *e) {
  for (stmt_t *s = c->stmts; s; s = s->next)
    if (s->entry == e) return 1;
  return 0;
}

static void conn_add_stmt(conn_t *c, sql_t *e) {
  stmt_t *s = calloc(1, sizeof(*s));
  if (!s) return;
  s->entry = e;
  s->next = c->stmts;
  c->stmts = s;
}

static void conn_clear_stmts(conn_t *c) {
  while (c->stmts) {
    stmt_t *s = c->stmts;
    c->stmts = s->next;
    free(s);
  }
}

static req_t *req_new(lunet_rt_t *rt, lua_State *co, int kind) {
  req_t *r = calloc(1, sizeof(*r));
  if (!r) return NULL;
  r->rt = rt;
  r->kind = kind;
  r->co_ref = lunet_co_ref(co, rt);
  return r;
}

static void req_free(req_t *r) {
  if (!r) return;
  if (r->co_ref != LUA_NOREF) luaL_unref(lunet_rt_L(r->rt), LUA_REGISTRYINDEX, r->co_ref);
  for (int i = 0; i < r->nparams; i++) free(r->params[i]);
  free(r->params);
  free(r->sql);
  free(r->name);
  free(r);
}

static int encode_params(lua_State *L, int first, int n, req_t *r) {
  if (n <= 0) return 0;
  r->params = calloc((size_t)n, sizeof(char *));
  if (!r->params) return -1;
  r->nparams = n;
  for (int i = 0; i < n; i++) {
    int idx = first + i;
    int t = lua_type(L, idx);
    if (t == LUA_TNIL || t == LUA_TNONE) continue;
    char buf[64];
    switch (t) {
      case LUA_TBOOLEAN:
        r->params[i] = dup_str(lua_toboolean(L, idx) ? "t" : "f");
        break;
      case LUA_TNUMBER: {
        lua_Number v = lua_tonumber(L, idx);
        if (v >= -PSQL_MAX_SAFE_INT && v <= PSQL_MAX_SAFE_INT && v == (double)(long long)v)
          snprintf(buf, sizeof(buf), "%lld", (long long)v);
        else
          snprintf(buf, sizeof(buf), "%.17g", (double)v);
        r->params[i] = dup_str(buf);
        break;
      }
      case LUA_TSTRING: {
        size_t len = 0;
        const char *s = lua_tolstring(L, idx, &len);
        if (strlen(s) != len) return -2;
        r->params[i] = dup_len(s, len);
        break;
      }
      default:
        return -2;
    }
    if (!r->params[i]) return -1;
  }
  return 0;
}

static lua_State *req_co(req_t *r) {
  lua_State *co = lunet_co_fetch(lunet_rt_L(r->rt), r->co_ref);
  r->co_ref = LUA_NOREF;
  return co;
}

static void req_end(req_t *r, lua_State *co, int nres) {
  lunet_co_resume(lunet_rt_L(r->rt), co, nres);
  req_free(r);
}

static void req_finish_error(req_t *r, const char *msg) {
  lua_State *co = req_co(r);
  if (co) {
    lua_pushnil(co);
    lua_pushstring(co, msg);
    req_end(r, co, 2);
  } else {
    req_free(r);
  }
}

static req_t *pop_waiter(pool_t *p) {
  req_t *w = p->wait_head;
  if (w) {
    p->wait_head = w->next;
    if (!p->wait_head) p->wait_tail = NULL;
    w->next = NULL;
  }
  return w;
}

static void push_waiter(pool_t *p, req_t *w) {
  w->next = NULL;
  if (p->wait_tail) p->wait_tail->next = w;
  else p->wait_head = w;
  p->wait_tail = w;
}

static void conn_recycle(conn_t *c) {
  c->next = c->pool->free_list;
  c->pool->free_list = c;
}

static void conn_closed_cb(uv_handle_t *h) {
  conn_recycle((conn_t *)h->data);
}

static void conn_discard(conn_t *c) {
  pool_t *p = c->pool;
  if (c->poll_init) uv_poll_stop(&c->poll);
  if (c->result) {
    PQclear(c->result);
    c->result = NULL;
  }
  if (c->pg) {
    PQfinish(c->pg);
    c->pg = NULL;
  }
  conn_clear_stmts(c);
  c->req = NULL;
  c->acquired = 0;
  c->orphaned = 0;
  c->stage = STAGE_NONE;
  c->state = CONN_FREE;
  p->count--;
  if (c->poll_init) {
    c->poll_init = 0;
    uv_close((uv_handle_t *)&c->poll, conn_closed_cb);
  } else {
    conn_recycle(c);
  }
}

static conn_t *conn_alloc(pool_t *p) {
  conn_t *c = p->free_list;
  if (c) {
    p->free_list = c->next;
    c->next = NULL;
    return c;
  }
  c = calloc(1, sizeof(*c));
  if (c) c->pool = p;
  return c;
}

static char *conn_abort(conn_t *c, const char *msg) {
  char *m = dup_str(msg ? msg : "connection error");
  c->req = NULL;
  conn_discard(c);
  return m;
}

static void set_events(conn_t *c, int events) {
  uv_poll_start(&c->poll, events, poll_cb);
}

static const char *conn_flush(conn_t *c) {
  int fr = PQflush(c->pg);
  if (fr < 0) return PQerrorMessage(c->pg);
  set_events(c, UV_READABLE | (fr ? UV_WRITABLE : 0));
  return NULL;
}

static const char *conn_send_run(conn_t *c, req_t *r) {
  int ok;
  if (r->name)
    ok = PQsendQueryPrepared(c->pg, r->name, r->nparams, (const char *const *)r->params, NULL, NULL, 0);
  else
    ok = PQsendQueryParams(c->pg, r->sql, r->nparams, NULL, (const char *const *)r->params, NULL, NULL, 0);
  return ok ? NULL : PQerrorMessage(c->pg);
}

static const char *conn_begin(conn_t *c, req_t *r) {
  c->req = r;
  c->state = CONN_LEASED;
  if (r->kind == REQ_EXEC) {
    sql_t *e = pool_find_entry(c->pool, r->name);
    if (!e) return "unknown prepared statement";
    if (!conn_has_stmt(c, e)) {
      if (!PQsendPrepare(c->pg, r->name, e->sql, 0, NULL)) return PQerrorMessage(c->pg);
      c->stage = STAGE_PREPARE;
      return conn_flush(c);
    }
  }
  c->stage = STAGE_RUN;
  const char *err = conn_send_run(c, r);
  if (err) return err;
  return conn_flush(c);
}

static void push_conn(lua_State *L, conn_t *c) {
  conn_handle_t *h = (conn_handle_t *)lua_newuserdata(L, sizeof(*h));
  h->c = c;
  h->gen = c->gen;
  luaL_getmetatable(L, PSQL_CONN_MT);
  lua_setmetatable(L, -2);
}

static void conn_lease(conn_t *c) {
  c->state = CONN_LEASED;
  c->acquired = 1;
  c->gen++;
  c->req = NULL;
  c->orphaned = 0;
  if (c->poll_init) uv_poll_stop(&c->poll);
}

static void conn_finish_req(conn_t *c, req_t *r) {
  c->req = NULL;
  c->stage = STAGE_NONE;
  if (c->poll_init) uv_poll_stop(&c->poll);
  if (r->release || c->orphaned) conn_release(c);
  else c->state = CONN_LEASED;
}

static void conn_release(conn_t *c) {
  pool_t *p = c->pool;
  c->req = NULL;
  c->stage = STAGE_NONE;
  c->acquired = 0;
  c->orphaned = 0;
  if (c->poll_init) uv_poll_stop(&c->poll);
  if (p->closed || !c->pg || PQstatus(c->pg) == CONNECTION_BAD) {
    conn_discard(c);
    pool_pump(p);
    return;
  }
  c->state = CONN_IDLE;
  if (p->wait_head) {
    conn_assign(c, pop_waiter(p));
    return;
  }
  c->next = p->idle;
  p->idle = c;
}

static void finish_acquire(conn_t *c, req_t *w) {
  lua_State *co = req_co(w);
  if (!co) {
    req_free(w);
    conn_release(c);
    return;
  }
  push_conn(co, c);
  req_end(w, co, 1);
}

static void conn_assign(conn_t *c, req_t *w) {
  c->req = w;
  if (c->state == CONN_CONNECTING) return;
  if (w->kind == REQ_ACQUIRE) {
    conn_lease(c);
    finish_acquire(c, w);
    return;
  }
  w->release = 1;
  const char *err = conn_begin(c, w);
  if (err) conn_fail(c, err);
}

static void conn_fail(conn_t *c, const char *msg) {
  pool_t *p = c->pool;
  req_t *r = c->req;
  char *m = conn_abort(c, msg);
  if (r) req_finish_error(r, m ? m : "out of memory");
  free(m);
  pool_pump(p);
}

static const char *conn_start_connect(conn_t *c) {
  pool_t *p = c->pool;
  c->pg = PQconnectStartParams((const char *const *)p->kw, (const char *const *)p->val, 1);
  if (!c->pg) return "out of memory";
  if (PQstatus(c->pg) == CONNECTION_BAD) return PQerrorMessage(c->pg);
  if (uv_poll_init(lunet_rt_loop(p->rt), &c->poll, PQsocket(c->pg)) != 0)
    return "failed to initialize socket watcher";
  c->poll.data = c;
  c->poll_init = 1;
  c->state = CONN_CONNECTING;
  c->stage = STAGE_CONNECT;
  set_events(c, UV_WRITABLE);
  return NULL;
}

static void conn_connect_step(conn_t *c) {
  switch (PQconnectPoll(c->pg)) {
    case PGRES_POLLING_READING:
      set_events(c, UV_READABLE);
      return;
    case PGRES_POLLING_WRITING:
      set_events(c, UV_WRITABLE);
      return;
    case PGRES_POLLING_FAILED:
      conn_fail(c, PQerrorMessage(c->pg));
      return;
    default:
      break;
  }
  if (PQsetnonblocking(c->pg, 1) != 0) {
    conn_fail(c, "failed to set nonblocking mode");
    return;
  }
  c->state = CONN_IDLE;
  conn_assign(c, c->req);
}

static void conn_prepare_done(conn_t *c, PGresult *res) {
  req_t *r = c->req;
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    char *m = dup_str(PQresultErrorMessage(res));
    PQclear(res);
    conn_finish_req(c, r);
    req_finish_error(r, m ? m : "prepare failed");
    free(m);
    return;
  }
  PQclear(res);
  conn_add_stmt(c, pool_find_entry(c->pool, r->name));
  c->stage = STAGE_RUN;
  const char *err = conn_send_run(c, r);
  if (!err) err = conn_flush(c);
  if (err) conn_fail(c, err);
}

static void conn_run_done(conn_t *c, PGresult *res) {
  req_t *r = c->req;
  lua_State *co = req_co(r);
  int n = 0;
  if (co) n = push_result(co, res);
  PQclear(res);
  conn_finish_req(c, r);
  if (co) req_end(r, co, n);
  else req_free(r);
}

static void conn_io_step(conn_t *c) {
  if (!c->req) {
    if (c->poll_init) uv_poll_stop(&c->poll);
    return;
  }
  int fr = PQflush(c->pg);
  if (fr < 0) {
    conn_fail(c, PQerrorMessage(c->pg));
    return;
  }
  if (!PQconsumeInput(c->pg)) {
    conn_fail(c, PQerrorMessage(c->pg));
    return;
  }
  for (;;) {
    if (PQisBusy(c->pg)) {
      set_events(c, UV_READABLE | (fr ? UV_WRITABLE : 0));
      return;
    }
    PGresult *res = PQgetResult(c->pg);
    if (!res) break;
    if (c->result) PQclear(c->result);
    c->result = res;
  }
  PGresult *res = c->result;
  c->result = NULL;
  if (!res) {
    conn_fail(c, "no result from server");
    return;
  }
  if (c->stage == STAGE_PREPARE) conn_prepare_done(c, res);
  else conn_run_done(c, res);
}

static void poll_cb(uv_poll_t *h, int status, int events) {
  conn_t *c = (conn_t *)h->data;
  (void)events;
  if (status < 0) {
    conn_fail(c, uv_strerror(status));
    return;
  }
  if (c->state == CONN_CONNECTING) conn_connect_step(c);
  else conn_io_step(c);
}

static conn_t *pool_take(pool_t *p, char **err) {
  *err = NULL;
  if (p->idle) {
    conn_t *c = p->idle;
    p->idle = c->next;
    c->next = NULL;
    return c;
  }
  if (p->count >= p->max_conns) return NULL;
  conn_t *c = conn_alloc(p);
  if (!c) {
    *err = dup_str("out of memory");
    return NULL;
  }
  p->count++;
  const char *e = conn_start_connect(c);
  if (e) {
    *err = conn_abort(c, e);
    return NULL;
  }
  return c;
}

static void pool_pump(pool_t *p) {
  if (p->closed) {
    req_t *w;
    while ((w = pop_waiter(p)) != NULL) req_finish_error(w, "pool closed");
    return;
  }
  while (p->wait_head) {
    char *err = NULL;
    conn_t *c = pool_take(p, &err);
    if (err) {
      req_t *w = pop_waiter(p);
      req_finish_error(w, err);
      free(err);
      continue;
    }
    if (!c) return;
    conn_assign(c, pop_waiter(p));
  }
}

static int pool_start(lua_State *L, pool_t *p, req_t *r) {
  if (!p->wait_head) {
    char *err = NULL;
    conn_t *c = pool_take(p, &err);
    if (err) {
      pool_pump(p);
      req_free(r);
      return push_owned_err(L, err);
    }
    if (c) {
      c->req = r;
      if (c->state == CONN_CONNECTING) return lua_yield(L, 0);
      const char *e = conn_begin(c, r);
      if (!e) return lua_yield(L, 0);
      char *m = conn_abort(c, e);
      pool_pump(p);
      req_free(r);
      return push_owned_err(L, m);
    }
  }
  push_waiter(p, r);
  return lua_yield(L, 0);
}

static pool_t *pool_check(lua_State *L, int idx) {
  pool_t **pp = (pool_t **)luaL_checkudata(L, idx, PSQL_POOL_MT);
  pool_t *p = *pp;
  if (p->closed) luaL_error(L, "psql pool is closed");
  if (lunet_rt_current() != p->rt) luaL_error(L, "psql pool used from another thread");
  return p;
}

static conn_t *conn_check(lua_State *L, int idx) {
  conn_handle_t *h = (conn_handle_t *)luaL_checkudata(L, idx, PSQL_CONN_MT);
  conn_t *c = h->c;
  if (!c || !c->acquired || c->gen != h->gen) luaL_error(L, "psql connection is released");
  if (lunet_rt_current() != c->pool->rt) luaL_error(L, "psql connection used from another thread");
  return c;
}

static int run_query(lua_State *L, int kind, pool_t *p, conn_t *c) {
  lunet_ensure_coroutine(L, "psql query");
  const char *s = luaL_checkstring(L, 2);
  if (kind == REQ_EXEC && !pool_find_entry(p, s)) return push_err(L, "unknown prepared statement");
  req_t *r = req_new(p->rt, L, kind);
  if (!r) return push_err(L, "out of memory");
  if (kind == REQ_EXEC) r->name = dup_str(s);
  else r->sql = dup_str(s);
  int e = encode_params(L, 3, lua_gettop(L) - 2, r);
  if (e != 0 || (!r->name && !r->sql)) {
    req_free(r);
    return push_err(L, e == -2 ? "unsupported parameter type or embedded NUL" : "out of memory");
  }
  if (c) {
    if (c->req) {
      req_free(r);
      return push_err(L, "connection is busy");
    }
    r->release = 0;
    const char *err = conn_begin(c, r);
    if (err) {
      char *m = conn_abort(c, err);
      pool_pump(p);
      req_free(r);
      return push_owned_err(L, m);
    }
    return lua_yield(L, 0);
  }
  r->release = 1;
  return pool_start(L, p, r);
}

static int l_pool_query(lua_State *L) {
  return run_query(L, REQ_QUERY, pool_check(L, 1), NULL);
}

static int l_pool_execute(lua_State *L) {
  return run_query(L, REQ_EXEC, pool_check(L, 1), NULL);
}

static int l_pool_prepare(lua_State *L) {
  pool_t *p = pool_check(L, 1);
  const char *name = luaL_checkstring(L, 2);
  const char *sql = luaL_checkstring(L, 3);
  sql_t *e = pool_find_entry(p, name);
  if (e) {
    if (strcmp(e->sql, sql) == 0) {
      lua_pushboolean(L, 1);
      return 1;
    }
    return push_err(L, "statement already registered with different sql");
  }
  e = calloc(1, sizeof(*e));
  if (!e) return push_err(L, "out of memory");
  e->name = dup_str(name);
  e->sql = dup_str(sql);
  if (!e->name || !e->sql) {
    free(e->name);
    free(e->sql);
    free(e);
    return push_err(L, "out of memory");
  }
  e->next = p->stmts;
  p->stmts = e;
  lua_pushboolean(L, 1);
  return 1;
}

static int l_pool_acquire(lua_State *L) {
  pool_t *p = pool_check(L, 1);
  lunet_ensure_coroutine(L, "psql acquire");
  req_t *r = req_new(p->rt, L, REQ_ACQUIRE);
  if (!r) return push_err(L, "out of memory");
  if (!p->wait_head) {
    char *err = NULL;
    conn_t *c = pool_take(p, &err);
    if (err) {
      pool_pump(p);
      req_free(r);
      return push_owned_err(L, err);
    }
    if (c && c->state == CONN_IDLE) {
      conn_lease(c);
      req_free(r);
      push_conn(L, c);
      return 1;
    }
    if (c) {
      c->req = r;
      return lua_yield(L, 0);
    }
  }
  push_waiter(p, r);
  return lua_yield(L, 0);
}

static int l_pool_close(lua_State *L) {
  pool_t **pp = (pool_t **)luaL_checkudata(L, 1, PSQL_POOL_MT);
  pool_t *p = *pp;
  if (p->closed) return 0;
  p->closed = 1;
  while (p->idle) {
    conn_t *c = p->idle;
    p->idle = c->next;
    c->next = NULL;
    conn_discard(c);
  }
  pool_pump(p);
  return 0;
}

static int l_conn_query(lua_State *L) {
  conn_t *c = conn_check(L, 1);
  return run_query(L, REQ_QUERY, c->pool, c);
}

static int l_conn_execute(lua_State *L) {
  conn_t *c = conn_check(L, 1);
  return run_query(L, REQ_EXEC, c->pool, c);
}

static int l_conn_release(lua_State *L) {
  conn_t *c = conn_check(L, 1);
  if (c->req) return luaL_error(L, "connection is busy");
  conn_release(c);
  return 0;
}

static void deferred_release(lunet_rt_t *rt, void *arg) {
  (void)rt;
  conn_t *c = (conn_t *)arg;
  if (c->orphaned && c->acquired && !c->req) conn_release(c);
}

static int l_conn_gc(lua_State *L) {
  conn_handle_t *h = (conn_handle_t *)luaL_checkudata(L, 1, PSQL_CONN_MT);
  conn_t *c = h->c;
  h->c = NULL;
  if (!c || !c->acquired || c->gen != h->gen) return 0;
  if (lunet_rt_current() != c->pool->rt) {
    if (c->pg) PQfinish(c->pg);
    return 0;
  }
  c->orphaned = 1;
  if (!c->req) lunet_rt_post(c->pool->rt, deferred_release, c);
  return 0;
}

static int l_psql_pool(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lunet_rt_t *rt = lunet_rt_current();
  if (!rt) return push_err(L, "psql.pool: no runtime in this thread");
  pool_t *p = calloc(1, sizeof(*p));
  if (!p) return push_err(L, "out of memory");
  p->rt = rt;
  p->max_conns = 10;

  int total = 0;
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {
    total++;
    lua_pop(L, 1);
  }
  p->kw = calloc((size_t)total + 1, sizeof(char *));
  p->val = calloc((size_t)total + 1, sizeof(char *));
  if (!p->kw || !p->val) {
    free(p->kw);
    free(p->val);
    free(p);
    return push_err(L, "out of memory");
  }

  int n = 0;
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {
    if (lua_type(L, -2) != LUA_TSTRING) return push_err(L, "psql.pool: option names must be strings");
    const char *key = lua_tostring(L, -2);
    int vt = lua_type(L, -1);
    if (strcmp(key, "max_conns") == 0) {
      p->max_conns = (int)lua_tonumber(L, -1);
    } else {
      if (vt != LUA_TSTRING && vt != LUA_TNUMBER) return push_err(L, "psql.pool: option values must be strings or numbers");
      p->kw[n] = dup_str(key);
      p->val[n] = dup_str(lua_tostring(L, -1));
      n++;
    }
    lua_pop(L, 1);
  }
  if (p->max_conns < 1) p->max_conns = 1;

  pool_t **pp = (pool_t **)lua_newuserdata(L, sizeof(pool_t *));
  *pp = p;
  luaL_getmetatable(L, PSQL_POOL_MT);
  lua_setmetatable(L, -2);
  return 1;
}

static void register_mt(lua_State *L, const char *name, const luaL_Reg *methods, lua_CFunction gc) {
  luaL_newmetatable(L, name);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  for (const luaL_Reg *r = methods; r->name; r++) {
    lua_pushcfunction(L, r->func);
    lua_setfield(L, -2, r->name);
  }
  if (gc) {
    lua_pushcfunction(L, gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_pop(L, 1);
}

static const luaL_Reg pool_methods[] = {
  {"query", l_pool_query},
  {"execute", l_pool_execute},
  {"prepare", l_pool_prepare},
  {"acquire", l_pool_acquire},
  {"close", l_pool_close},
  {NULL, NULL}
};

static const luaL_Reg conn_methods[] = {
  {"query", l_conn_query},
  {"execute", l_conn_execute},
  {"release", l_conn_release},
  {NULL, NULL}
};

int lunet_open_psql(lua_State *L) {
  register_mt(L, PSQL_POOL_MT, pool_methods, NULL);
  register_mt(L, PSQL_CONN_MT, conn_methods, l_conn_gc);
  static const luaL_Reg funcs[] = {
    {"pool", l_psql_pool},
    {NULL, NULL}
  };
  luaL_newlib(L, funcs);
  return 1;
}