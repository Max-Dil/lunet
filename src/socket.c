#include "socket.h"

#include <arpa/inet.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <uv.h>

#include "co.h"
#include "rt.h"  
#include "stl.h"

static size_t read_buffer_size = 4096;

typedef enum {
  TCP_SERVER,
  TCP_CLIENT,
} tcp_type_t;

typedef struct {
  uv_tcp_t handle;
  tcp_type_t type;
  int closed;  
  union {
    struct {
      int accept_ref;
      queue_t *pending_accepts;
    } server;
    struct {
      int read_ref;            
      int read_cb_ref;         
      uv_timer_t *read_timer;  
    } client;
  };
} tcp_ctx_t;

typedef struct {
  uv_write_t req;
  tcp_ctx_t *ctx;
  char *data;
  int co_ref;  
  int cb_ref;  
} write_req_t;

static void lunet_close_cb(uv_handle_t *handle) {
  tcp_ctx_t *ctx = (tcp_ctx_t *)handle->data;
  if (ctx) {
    if (ctx->type == TCP_SERVER) {
      queue_destroy(ctx->server.pending_accepts);
    }
    free(ctx);
  }
}

static void lunet_timer_close_cb(uv_handle_t *handle) { free(handle); }

static void resume_waiter(lua_State *co, int nargs, const char *where) {
  int st = lua_resume(co, nargs);
  if (st != LUA_OK && st != LUA_YIELD) {
    const char *err = lua_tostring(co, -1);
    fprintf(stderr, "[lunet] resume error in %s: %s\n", where, err ? err : "?");
  }
}

static void call_callback(lua_State *L, int nargs, const char *where) {
  if (lua_pcall(L, nargs, 0, 0) != 0) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "[lunet] error in %s callback: %s\n", where, err ? err : "?");
    lua_pop(L, 1);
  }
}

static void push_read_result(lua_State *L, const char *data, size_t len, const char *err) {
  if (data) {
    lua_pushlstring(L, data, len);
    lua_pushnil(L);
  } else {
    lua_pushnil(L);
    if (err) {
      lua_pushstring(L, err);
    } else {
      lua_pushnil(L);  
    }
  }
}

static void lunet_write_cb(uv_write_t *req, int status) {
  write_req_t *wr = (write_req_t *)req;
  lua_State *L = default_luaL();

  if (wr->cb_ref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, wr->cb_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, wr->cb_ref);
    if (status == 0) {
      lua_pushnil(L);
    } else {
      lua_pushstring(L, uv_strerror(status));
    }
    call_callback(L, 1, "write");
  } else if (wr->co_ref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, wr->co_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, wr->co_ref);
    if (lua_isthread(L, -1)) {
      lua_State *waiting_co = lua_tothread(L, -1);
      if (status == 0) {
        lua_pushnil(waiting_co);
      } else {
        lua_pushstring(waiting_co, uv_strerror(status));
      }
      resume_waiter(waiting_co, 1, "write_cb");
    }
    lua_pop(L, 1);  
  }

  free(wr->data);
  free(wr);
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(read_buffer_size);
  buf->len = buf->base ? read_buffer_size : 0;
}

static void lunet_read_finish(tcp_ctx_t *ctx, const char *data, size_t len, const char *err) {
  int co_ref = ctx->client.read_ref;
  int cb_ref = ctx->client.read_cb_ref;
  if (co_ref == LUA_NOREF && cb_ref == LUA_NOREF) {
    return;
  }
  ctx->client.read_ref = LUA_NOREF;
  ctx->client.read_cb_ref = LUA_NOREF;

  uv_read_stop((uv_stream_t *)&ctx->handle);
  if (ctx->client.read_timer) {
    uv_timer_stop(ctx->client.read_timer);
  }

  lua_State *L = default_luaL();
  if (cb_ref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, cb_ref);
    push_read_result(L, data, len, err);
    call_callback(L, 2, "read");
  } else {
    lua_rawgeti(L, LUA_REGISTRYINDEX, co_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
    if (lua_isthread(L, -1)) {
      lua_State *waiting_co = lua_tothread(L, -1);
      push_read_result(waiting_co, data, len, err);
      resume_waiter(waiting_co, 2, "read");
    }
    lua_pop(L, 1);
  }
}

static void lunet_read_timeout_cb(uv_timer_t *timer) {
  lunet_read_finish((tcp_ctx_t *)timer->data, NULL, 0, "timeout");
}

static void lunet_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  tcp_ctx_t *ctx = (tcp_ctx_t *)stream->data;

  if (nread == 0) {  
    free(buf->base);
    return;
  }

  if (nread > 0) {
    lunet_read_finish(ctx, buf->base, (size_t)nread, NULL);
  } else if (nread == UV_EOF) {
    lunet_read_finish(ctx, NULL, 0, NULL);
  } else {
    lunet_read_finish(ctx, NULL, 0, uv_strerror((int)nread));
  }

  free(buf->base);
}

static void lunet_accept_wake(tcp_ctx_t *ctx, tcp_ctx_t *client_ctx, const char *err) {
  lua_State *L = default_luaL();
  int ref = ctx->server.accept_ref;
  ctx->server.accept_ref = LUA_NOREF;

  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  luaL_unref(L, LUA_REGISTRYINDEX, ref);
  if (lua_isthread(L, -1)) {
    lua_State *waiting_co = lua_tothread(L, -1);
    if (client_ctx) {
      lua_pushlightuserdata(waiting_co, client_ctx);
      lua_pushnil(waiting_co);
    } else {
      lua_pushnil(waiting_co);
      lua_pushstring(waiting_co, err);
    }
    resume_waiter(waiting_co, 2, "listen_cb");
  }
  lua_pop(L, 1);
}

static void lunet_listen_cb(uv_stream_t *server, int status) {
  tcp_ctx_t *ctx = (tcp_ctx_t *)server->data;

  if (status < 0) {
    if (ctx->server.accept_ref != LUA_NOREF) {
      lunet_accept_wake(ctx, NULL, uv_strerror(status));
    }
    return;
  }

  tcp_ctx_t *client_ctx = malloc(sizeof(tcp_ctx_t));
  if (!client_ctx) {
    return;  
  }

  client_ctx->type = TCP_CLIENT;
  client_ctx->closed = 0;
  client_ctx->client.read_ref = LUA_NOREF;
  client_ctx->client.read_cb_ref = LUA_NOREF;
  client_ctx->client.read_timer = NULL;

  if (uv_tcp_init(uv_default_loop(), &client_ctx->handle) < 0) {
    free(client_ctx);
    return;
  }

  client_ctx->handle.data = client_ctx;

  if (uv_accept(server, (uv_stream_t *)&client_ctx->handle) < 0) {
    uv_close((uv_handle_t *)&client_ctx->handle, lunet_close_cb);
    return;
  }

  if (ctx->server.accept_ref != LUA_NOREF) {
    lunet_accept_wake(ctx, client_ctx, NULL);
  } else {
    if (queue_enqueue(ctx->server.pending_accepts, client_ctx) != 0) {
      uv_close((uv_handle_t *)&client_ctx->handle, lunet_close_cb);
    }
  }
}

int lunet_socket_listen(lua_State *co) {
  if (lunet_ensure_coroutine(co, "socket.listen") != 0) {
    return lua_error(co);
  }
  const char *protocol = luaL_checkstring(co, 1);
  const char *host = luaL_checkstring(co, 2);
  int port = luaL_checkinteger(co, 3);

  int reuse_port = lua_toboolean(co, 4);

  if (port < 1 || port > 65535) {
    lua_pushnil(co);
    lua_pushstring(co, "port must be between 1 and 65535");
    return 2;
  }
  if (strcmp(protocol, "tcp") != 0) {
    lua_pushnil(co);
    lua_pushstring(co, "only tcp is supported");
    return 2;
  }

  struct sockaddr_in addr;
  if (uv_ip4_addr(host, port, &addr) < 0) {
    lua_pushnil(co);
    lua_pushstring(co, "invalid host or port");
    return 2;
  }

  tcp_ctx_t *ctx = malloc(sizeof(tcp_ctx_t));
  if (!ctx) {
    lua_pushnil(co);
    lua_pushstring(co, "out of memory");
    return 2;
  }
  ctx->type = TCP_SERVER;
  ctx->closed = 0;
  ctx->server.accept_ref = LUA_NOREF;
  ctx->server.pending_accepts = queue_init();
  if (!ctx->server.pending_accepts) {
    free(ctx);
    lua_pushnil(co);
    lua_pushstring(co, "out of memory");
    return 2;
  }

  int ret = 0;
  if ((ret = uv_tcp_init_ex(uv_default_loop(), &ctx->handle, AF_INET)) < 0) {
    queue_destroy(ctx->server.pending_accepts);
    free(ctx);
    lua_pushnil(co);
    lua_pushfstring(co, "failed to initialize TCP: %s", uv_strerror(ret));
    return 2;
  }

  ctx->handle.data = ctx;

  if (reuse_port) {
    uv_os_fd_t fd;
    if (uv_fileno((const uv_handle_t *)&ctx->handle, &fd) == 0) {
#ifdef SO_REUSEPORT
      int optval = 1;
      setsockopt((int)fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
#else
      fprintf(stderr, "[lunet] warning: SO_REUSEPORT is not supported on this platform\n");
#endif
    }
  }

  if ((ret = uv_tcp_bind(&ctx->handle, (const struct sockaddr *)&addr, 0)) < 0) {
    uv_close((uv_handle_t *)&ctx->handle, lunet_close_cb);
    lua_pushnil(co);
    lua_pushfstring(co, "failed to bind: %s", uv_strerror(ret));
    return 2;
  }

  if ((ret = uv_listen((uv_stream_t *)&ctx->handle, 128, lunet_listen_cb)) < 0) {
    uv_close((uv_handle_t *)&ctx->handle, lunet_close_cb);
    lua_pushnil(co);
    lua_pushfstring(co, "failed to listen: %s", uv_strerror(ret));
    return 2;
  }

  lua_pushlightuserdata(co, ctx);
  lua_pushnil(co);
  return 2;
}

int lunet_socket_accept(lua_State *co) {
  if (lunet_ensure_coroutine(co, "socket.accept") != 0) {
    return lua_error(co);
  }

  if (!lua_islightuserdata(co, 1)) {
    lua_pushnil(co);
    lua_pushstring(co, "invalid listener handle");
    return 2;
  }

  tcp_ctx_t *listener_ctx = (tcp_ctx_t *)lua_touserdata(co, 1);
  if (!listener_ctx || listener_ctx->type != TCP_SERVER) {
    lua_pushnil(co);
    lua_pushstring(co, "invalid listener handle");
    return 2;
  }

  if (listener_ctx->server.accept_ref != LUA_NOREF) {
    lua_pushnil(co);
    lua_pushstring(co, "another accept already in progress");
    return 2;
  }

  if (!queue_is_empty(listener_ctx->server.pending_accepts)) {
    tcp_ctx_t *client_ctx = (tcp_ctx_t *)queue_dequeue(listener_ctx->server.pending_accepts);
    if (client_ctx) {
      lua_pushlightuserdata(co, client_ctx);
      lua_pushnil(co);
      return 2;
    }
  }

  lua_pushthread(co);
  listener_ctx->server.accept_ref = luaL_ref(co, LUA_REGISTRYINDEX);

  return lua_yield(co, 0);
}

int lunet_socket_getpeername(lua_State *L) {
  if (lunet_ensure_coroutine(L, "socket.getpeername") != 0) {
    return lua_error(L);
  }

  if (!lua_islightuserdata(L, 1)) {
    lua_pushnil(L);
    lua_pushstring(L, "invalid socket handle");
    return 2;
  }

  tcp_ctx_t *ctx = (tcp_ctx_t *)lua_touserdata(L, 1);
  if (!ctx) {
    lua_pushnil(L);
    lua_pushstring(L, "invalid socket handle");
    return 2;
  }

  struct sockaddr_in addr;
  int addr_len = sizeof(addr);
  int ret = uv_tcp_getpeername(&ctx->handle, (struct sockaddr *)&addr, &addr_len);
  if (ret < 0) {
    lua_pushnil(L);
    lua_pushfstring(L, "failed to get peer name: %s", uv_strerror(ret));
    return 2;
  }

  char buf[INET_ADDRSTRLEN];
  if (uv_ip4_name(&addr, buf, sizeof(buf)) < 0) {
    lua_pushnil(L);
    lua_pushstring(L, "failed to get peer name");
    return 2;
  }
  lua_pushstring(L, buf);
  lua_pushinteger(L, ntohs(addr.sin_port));

  return 2;
}

int lunet_socket_close(lua_State *L) {
  if (!lua_islightuserdata(L, 1)) {
    lua_pushstring(L, "invalid socket handle");
    return 1;
  }

  tcp_ctx_t *ctx = (tcp_ctx_t *)lua_touserdata(L, 1);
  if (!ctx) {
    lua_pushstring(L, "invalid socket handle");
    return 1;
  }

  if (ctx->closed) {
    lua_pushstring(L, "socket already closed");
    return 1;
  }
  ctx->closed = 1;

  if (ctx->type == TCP_CLIENT) {
    lunet_read_finish(ctx, NULL, 0, "socket closed");
    if (ctx->client.read_timer) {
      uv_close((uv_handle_t *)ctx->client.read_timer, lunet_timer_close_cb);
      ctx->client.read_timer = NULL;
    }
  }

  uv_close((uv_handle_t *)&ctx->handle, lunet_close_cb);

  lua_pushnil(L);
  return 1;
}

int lunet_socket_read(lua_State *L) {
  int has_cb = !lua_isnoneornil(L, 3);
  if (has_cb && !lua_isfunction(L, 3)) {
    lua_pushnil(L);
    lua_pushstring(L, "callback must be a function");
    return 2;
  }
  if (!has_cb && lunet_ensure_coroutine(L, "socket.read") != 0) {
    return lua_error(L);
  }

  if (!lua_islightuserdata(L, 1)) {
    lua_pushnil(L);
    lua_pushstring(L, "invalid socket handle");
    return 2;
  }

  tcp_ctx_t *ctx = (tcp_ctx_t *)lua_touserdata(L, 1);
  if (!ctx || ctx->type != TCP_CLIENT) {
    lua_pushnil(L);
    lua_pushstring(L, "invalid client socket handle");
    return 2;
  }
  if (ctx->closed) {
    lua_pushnil(L);
    lua_pushstring(L, "socket closed");
    return 2;
  }

  lua_Integer timeout = 0;  
  if (!lua_isnoneornil(L, 2)) {
    if (!lua_isnumber(L, 2) || lua_tointeger(L, 2) < 0) {
      lua_pushnil(L);
      lua_pushstring(L, "timeout must be a non-negative number (ms)");
      return 2;
    }
    timeout = lua_tointeger(L, 2);
  }

  if (ctx->client.read_ref != LUA_NOREF || ctx->client.read_cb_ref != LUA_NOREF) {
    lua_pushnil(L);
    lua_pushstring(L, "another read already in progress");
    return 2;
  }

  if (timeout > 0 && !ctx->client.read_timer) {
    uv_timer_t *t = malloc(sizeof(*t));
    if (!t) {
      lua_pushnil(L);
      lua_pushstring(L, "out of memory");
      return 2;
    }
    uv_timer_init(uv_default_loop(), t);
    t->data = ctx;
    ctx->client.read_timer = t;
  }

  int *ref = has_cb ? &ctx->client.read_cb_ref : &ctx->client.read_ref;
  if (has_cb) {
    lua_pushvalue(L, 3);
  } else {
    lua_pushthread(L);
  }
  *ref = luaL_ref(L, LUA_REGISTRYINDEX);

  int ret = uv_read_start((uv_stream_t *)&ctx->handle, alloc_buffer, lunet_read_cb);
  if (ret < 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, *ref);
    *ref = LUA_NOREF;

    lua_pushnil(L);
    lua_pushfstring(L, "failed to start reading: %s", uv_strerror(ret));
    return 2;
  }

  if (timeout > 0) {
    uv_timer_start(ctx->client.read_timer, lunet_read_timeout_cb, (uint64_t)timeout, 0);
  }

  if (has_cb) {
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
  }

  return lua_yield(L, 0);
}

int lunet_socket_write(lua_State *L) {
  int wait = lua_isnoneornil(L, 3) ? 1 : lua_toboolean(L, 3);
  int has_cb = !lua_isnoneornil(L, 4);
  if (has_cb && !lua_isfunction(L, 4)) {
    lua_pushstring(L, "callback must be a function");
    return 1;
  }

  int yield_mode = wait && !has_cb;
  if (yield_mode && lunet_ensure_coroutine(L, "socket.write") != 0) {
    return lua_error(L);
  }

  if (!lua_islightuserdata(L, 1)) {
    lua_pushstring(L, "invalid socket handle");
    return 1;
  }

  if (!lua_isstring(L, 2)) {
    lua_pushstring(L, "data must be a string");
    return 1;
  }

  tcp_ctx_t *ctx = (tcp_ctx_t *)lua_touserdata(L, 1);
  if (!ctx || ctx->type != TCP_CLIENT) {
    lua_pushstring(L, "invalid client socket handle");
    return 1;
  }
  if (ctx->closed) {
    lua_pushstring(L, "socket closed");
    return 1;
  }

  size_t data_len;
  const char *data = lua_tolstring(L, 2, &data_len);

  write_req_t *wr = malloc(sizeof(write_req_t));
  if (!wr) {
    lua_pushstring(L, "out of memory");
    return 1;
  }

  wr->data = malloc(data_len ? data_len : 1);
  if (!wr->data) {
    free(wr);
    lua_pushstring(L, "out of memory");
    return 1;
  }
  memcpy(wr->data, data, data_len);

  wr->ctx = ctx;
  wr->co_ref = LUA_NOREF;
  wr->cb_ref = LUA_NOREF;

  uv_buf_t buf = uv_buf_init(wr->data, (unsigned int)data_len);

  int ret = uv_write(&wr->req, (uv_stream_t *)&ctx->handle, &buf, 1, lunet_write_cb);
  if (ret < 0) {
    free(wr->data);
    free(wr);
    lua_pushfstring(L, "failed to start writing: %s", uv_strerror(ret));
    return 1;
  }

  if (has_cb) {
    lua_pushvalue(L, 4);
    wr->cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  } else if (wait) {
    lua_pushthread(L);
    wr->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return lua_yield(L, 0);
  }

  lua_pushnil(L);
  return 1;
}

typedef struct {
  uv_connect_t req;
  tcp_ctx_t *ctx;
  int co_ref;
} connect_ctx_t;

static void lunet_connect_cb(uv_connect_t *req, int status) {
  connect_ctx_t *cctx = (connect_ctx_t *)req->data;
  lua_State *L = default_luaL();

  lua_rawgeti(L, LUA_REGISTRYINDEX, cctx->co_ref);
  luaL_unref(L, LUA_REGISTRYINDEX, cctx->co_ref);
  cctx->co_ref = LUA_NOREF;

  if (lua_isthread(L, -1)) {
    lua_State *waiting_co = lua_tothread(L, -1);
    if (status == 0) {
      lua_pushlightuserdata(waiting_co, cctx->ctx);
      lua_pushnil(waiting_co);
    } else {
      lua_pushnil(waiting_co);
      lua_pushstring(waiting_co, uv_strerror(status));
    }
    resume_waiter(waiting_co, 2, "connect_cb");
  }
  lua_pop(L, 1);

  if (status != 0) {
    uv_close((uv_handle_t *)&cctx->ctx->handle, lunet_close_cb);
  }

  free(cctx);
}

int lunet_socket_connect(lua_State *L) {
  if (lunet_ensure_coroutine(L, "socket.connect") != 0) {
    return lua_error(L);
  }

  const char *host = luaL_checkstring(L, 1);
  int port = luaL_checkinteger(L, 2);

  if (port < 1 || port > 65535) {
    lua_pushnil(L);
    lua_pushstring(L, "port must be between 1 and 65535");
    return 2;
  }

  tcp_ctx_t *ctx = malloc(sizeof(tcp_ctx_t));
  if (!ctx) {
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }

  ctx->type = TCP_CLIENT;
  ctx->closed = 0;
  ctx->client.read_ref = LUA_NOREF;
  ctx->client.read_cb_ref = LUA_NOREF;
  ctx->client.read_timer = NULL;

  int ret = uv_tcp_init(uv_default_loop(), &ctx->handle);
  if (ret < 0) {
    free(ctx);
    lua_pushnil(L);
    lua_pushfstring(L, "failed to initialize TCP: %s", uv_strerror(ret));
    return 2;
  }

  ctx->handle.data = ctx;

  struct sockaddr_in dest;
  ret = uv_ip4_addr(host, port, &dest);
  if (ret < 0) {
    uv_close((uv_handle_t *)&ctx->handle, lunet_close_cb);
    lua_pushnil(L);
    lua_pushstring(L, "invalid host or port");
    return 2;
  }

  connect_ctx_t *connect_ctx = malloc(sizeof(connect_ctx_t));
  if (!connect_ctx) {
    uv_close((uv_handle_t *)&ctx->handle, lunet_close_cb);
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }

  connect_ctx->ctx = ctx;
  connect_ctx->co_ref = LUA_NOREF;
  connect_ctx->req.data = connect_ctx;

  lua_pushthread(L);
  connect_ctx->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  ret = uv_tcp_connect(&connect_ctx->req, &ctx->handle, (const struct sockaddr *)&dest, lunet_connect_cb);
  if (ret < 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, connect_ctx->co_ref);
    connect_ctx->co_ref = LUA_NOREF;
    free(connect_ctx);
    uv_close((uv_handle_t *)&ctx->handle, lunet_close_cb);
    lua_pushnil(L);
    lua_pushfstring(L, "failed to start connect: %s", uv_strerror(ret));
    return 2;
  }

  return lua_yield(L, 0);
}

int lunet_socket_set_read_buffer_size(lua_State *L) {
  if (lua_isnumber(L, 1)) {
    lua_Integer size = lua_tointeger(L, 1);
    if (size > 0) {
      read_buffer_size = (size_t)size;
    }
  }
  lua_pushnil(L);
  return 1;
}