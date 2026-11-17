#include "fs.h"

#include <fcntl.h>
#include <lauxlib.h>
#include <lua.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "co.h"

typedef struct {
  uv_fs_t req;
  lua_State *L;
  int co_ref;
  char *buf;
} fs_ctx_t;

static void fs_ctx_free(fs_ctx_t *ctx) {
  uv_fs_req_cleanup(&ctx->req);
  free(ctx->buf);
  free(ctx);
}

static lua_State *fs_get_co(lua_State *L, int ref, const char *name) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  lua_State *co = lua_isthread(L, -1) ? lua_tothread(L, -1) : NULL;
  lua_pop(L, 1);

  if (!co || lua_status(co) != LUA_YIELD) {
    fprintf(stderr, "invalid or non-yielded coroutine in %s\n", name);
    return NULL;
  }
  if (!lua_checkstack(co, 8)) {
    fprintf(stderr, "stack overflow in %s\n", name);
    return NULL;
  }
  return co;
}

static void fs_resume(lua_State *co, int nargs, const char *name) {
  int st = lua_resume(co, nargs);
  if (st != 0 && st != LUA_YIELD) {
    const char *msg = lua_tostring(co, -1);
    fprintf(stderr, "error in coroutine after %s: %s\n", name,
            msg ? msg : "(non-string error)");
  }
}

static void fs_ctx_abort(fs_ctx_t *ctx) {
  luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->co_ref);
  fs_ctx_free(ctx);
}

static fs_ctx_t *fs_ctx_new(lua_State *L) {
  fs_ctx_t *ctx = calloc(1, sizeof(fs_ctx_t));
  if (!ctx) return NULL;
  ctx->L = L;
  lua_pushthread(L);
  ctx->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  ctx->req.data = ctx;
  return ctx;
}

static void lunet_fs_open_cb(uv_fs_t *req) {
  fs_ctx_t *ctx = (fs_ctx_t *)req->data;
  lua_State *co = fs_get_co(ctx->L, ctx->co_ref, "fs.open");

  if (co) {
    if (req->result >= 0) {
      lua_pushinteger(co, req->result);
      lua_pushnil(co);
    } else {
      lua_pushnil(co);
      lua_pushstring(co, uv_strerror((int)req->result));
    }
    fs_resume(co, 2, "fs.open");
  }

  luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->co_ref);
  fs_ctx_free(ctx);
}

static int fs_mode_to_flags(const char *mode, size_t mode_len) {
  if (mode_len == 0) return -1;

  int flags = 0;
  bool has_plus = false;

  for (size_t i = 0; i < mode_len; i++) {
    if (mode[i] == '+') has_plus = true;
  }

  switch (mode[0]) {
    case 'r':
      flags = has_plus ? O_RDWR : O_RDONLY;
      break;
    case 'w':
      flags = (has_plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
      break;
    case 'a':
      flags = (has_plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
      break;
    case 'x':
      flags = (has_plus ? O_RDWR : O_WRONLY) | O_CREAT | O_EXCL;
      break;
    default:
      return -1;
  }
  return flags;
}

int lunet_fs_open(lua_State *L) {
  if (lunet_ensure_coroutine(L, "fs.open") != 0) {
    return lua_error(L);
  }
  if (lua_gettop(L) < 2) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.open requires path and mode");
    return 2;
  }

  const char *path = luaL_checkstring(L, 1);
  const char *mode = luaL_checkstring(L, 2);

  int flags = fs_mode_to_flags(mode, strlen(mode));
  if (flags == -1) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.open invalid mode");
    return 2;
  }

  fs_ctx_t *ctx = fs_ctx_new(L);
  if (!ctx) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.open: out of memory");
    return 2;
  }

  int rc = uv_fs_open(uv_default_loop(), &ctx->req, path, flags, 0644, lunet_fs_open_cb);
  if (rc < 0) {
    fs_ctx_abort(ctx);
    lua_pushnil(L);
    lua_pushstring(L, uv_strerror(rc));
    return 2;
  }

  return lua_yield(L, 0);
}

static void lunet_fs_close_cb(uv_fs_t *req) {
  fs_ctx_t *ctx = (fs_ctx_t *)req->data;
  lua_State *co = fs_get_co(ctx->L, ctx->co_ref, "fs.close");

  if (co) {
    if (req->result == 0) {
      lua_pushnil(co);
    } else {
      lua_pushstring(co, uv_strerror((int)req->result));
    }
    fs_resume(co, 1, "fs.close");
  }

  luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->co_ref);
  fs_ctx_free(ctx);
}

int lunet_fs_close(lua_State *L) {
  if (lunet_ensure_coroutine(L, "fs.close") != 0) {
    return lua_error(L);
  }
  if (lua_gettop(L) < 1 || !lua_isnumber(L, 1)) {
    lua_pushstring(L, "fs.close requires 1 integer fd");
    return 1;
  }

  uv_file fd = (uv_file)lua_tointeger(L, 1);

  fs_ctx_t *ctx = fs_ctx_new(L);
  if (!ctx) {
    lua_pushstring(L, "fs.close: out of memory");
    return 1;
  }

  int rc = uv_fs_close(uv_default_loop(), &ctx->req, fd, lunet_fs_close_cb);
  if (rc < 0) {
    fs_ctx_abort(ctx);
    lua_pushstring(L, uv_strerror(rc));
    return 1;
  }

  return lua_yield(L, 0);
}

static void lunet_fs_stat_cb(uv_fs_t *req) {
  fs_ctx_t *ctx = (fs_ctx_t *)req->data;
  lua_State *co = fs_get_co(ctx->L, ctx->co_ref, "fs.stat");

  if (co) {
    if (req->result == 0) {
      uv_stat_t *s = &req->statbuf;

      lua_newtable(co);
      lua_pushinteger(co, (lua_Integer)s->st_dev);
      lua_setfield(co, -2, "dev");
      lua_pushinteger(co, (lua_Integer)s->st_mode);
      lua_setfield(co, -2, "mode");
      lua_pushinteger(co, (lua_Integer)s->st_nlink);
      lua_setfield(co, -2, "nlink");
      lua_pushinteger(co, (lua_Integer)s->st_uid);
      lua_setfield(co, -2, "uid");
      lua_pushinteger(co, (lua_Integer)s->st_gid);
      lua_setfield(co, -2, "gid");
      lua_pushinteger(co, (lua_Integer)s->st_rdev);
      lua_setfield(co, -2, "rdev");
      lua_pushinteger(co, (lua_Integer)s->st_ino);
      lua_setfield(co, -2, "ino");
      lua_pushinteger(co, (lua_Integer)s->st_size);
      lua_setfield(co, -2, "size");
      lua_pushinteger(co, (lua_Integer)s->st_blksize);
      lua_setfield(co, -2, "blksize");
      lua_pushinteger(co, (lua_Integer)s->st_blocks);
      lua_setfield(co, -2, "blocks");
      lua_pushinteger(co, (lua_Integer)s->st_flags);
      lua_setfield(co, -2, "flags");
      lua_pushinteger(co, (lua_Integer)s->st_gen);
      lua_setfield(co, -2, "gen");
      lua_pushnumber(co, (lua_Number)s->st_atim.tv_sec);
      lua_setfield(co, -2, "atim");
      lua_pushnumber(co, (lua_Number)s->st_mtim.tv_sec);
      lua_setfield(co, -2, "mtim");
      lua_pushnumber(co, (lua_Number)s->st_ctim.tv_sec);
      lua_setfield(co, -2, "ctim");
      lua_pushnumber(co, (lua_Number)s->st_birthtim.tv_sec);
      lua_setfield(co, -2, "birthtim");
      lua_pushnil(co);
    } else {
      lua_pushnil(co);
      lua_pushstring(co, uv_strerror((int)req->result));
    }
    fs_resume(co, 2, "fs.stat");
  }

  luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->co_ref);
  fs_ctx_free(ctx);
}

int lunet_fs_stat(lua_State *L) {
  if (lunet_ensure_coroutine(L, "fs.stat") != 0) {
    return lua_error(L);
  }
  if (!lua_isstring(L, 1)) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.stat requires path");
    return 2;
  }

  const char *path = luaL_checkstring(L, 1);

  fs_ctx_t *ctx = fs_ctx_new(L);
  if (!ctx) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.stat out of memory");
    return 2;
  }

  int rc = uv_fs_stat(uv_default_loop(), &ctx->req, path, lunet_fs_stat_cb);
  if (rc < 0) {
    fs_ctx_abort(ctx);
    lua_pushnil(L);
    lua_pushstring(L, uv_strerror(rc));
    return 2;
  }

  return lua_yield(L, 0);
}

static void lunet_fs_read_cb(uv_fs_t *req) {
  fs_ctx_t *ctx = (fs_ctx_t *)req->data;
  lua_State *co = fs_get_co(ctx->L, ctx->co_ref, "fs.read");

  if (co) {
    if (req->result >= 0) {
      lua_pushlstring(co, ctx->buf, (size_t)req->result);
      lua_pushnil(co);
    } else {
      lua_pushnil(co);
      lua_pushstring(co, uv_strerror((int)req->result));
    }
    fs_resume(co, 2, "fs.read");
  }

  luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->co_ref);
  fs_ctx_free(ctx);
}

int lunet_fs_read(lua_State *L) {
  if (lunet_ensure_coroutine(L, "fs.read") != 0) {
    return lua_error(L);
  }
  if (lua_gettop(L) < 2 || !lua_isnumber(L, 1) || !lua_isnumber(L, 2)) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.read requires fd and length");
    return 2;
  }

  uv_file fd = (uv_file)lua_tointeger(L, 1);
  lua_Integer n = lua_tointeger(L, 2);
  if (n < 0) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.read: negative length");
    return 2;
  }
  if (n == 0) {
    lua_pushliteral(L, "");
    lua_pushnil(L);
    return 2;
  }
  size_t len = (size_t)n;

  fs_ctx_t *ctx = fs_ctx_new(L);
  if (!ctx) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.read out of memory");
    return 2;
  }

  ctx->buf = malloc(len);
  if (!ctx->buf) {
    fs_ctx_abort(ctx);
    lua_pushnil(L);
    lua_pushstring(L, "fs.read out of memory");
    return 2;
  }

  uv_buf_t buf = uv_buf_init(ctx->buf, (unsigned int)len);
  int rc = uv_fs_read(uv_default_loop(), &ctx->req, fd, &buf, 1, -1, lunet_fs_read_cb);
  if (rc < 0) {
    fs_ctx_abort(ctx);
    lua_pushnil(L);
    lua_pushstring(L, uv_strerror(rc));
    return 2;
  }

  return lua_yield(L, 0);
}

static void lunet_fs_write_cb(uv_fs_t *req) {
  fs_ctx_t *ctx = (fs_ctx_t *)req->data;
  lua_State *co = fs_get_co(ctx->L, ctx->co_ref, "fs.write");

  if (co) {
    if (req->result >= 0) {
      lua_pushinteger(co, req->result);
      lua_pushnil(co);
    } else {
      lua_pushnil(co);
      lua_pushstring(co, uv_strerror((int)req->result));
    }
    fs_resume(co, 2, "fs.write");
  }

  luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->co_ref);
  fs_ctx_free(ctx);
}

int lunet_fs_write(lua_State *L) {
  if (lunet_ensure_coroutine(L, "fs.write") != 0) {
    return lua_error(L);
  }
  if (lua_gettop(L) < 2 || !lua_isnumber(L, 1) || !lua_isstring(L, 2)) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.write requires fd and data");
    return 2;
  }

  uv_file fd = (uv_file)lua_tointeger(L, 1);
  size_t len;
  const char *data = luaL_checklstring(L, 2, &len);

  if (len == 0) {
    lua_pushinteger(L, 0);
    lua_pushnil(L);
    return 2;
  }

  fs_ctx_t *ctx = fs_ctx_new(L);
  if (!ctx) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.write out of memory");
    return 2;
  }

  ctx->buf = malloc(len);
  if (!ctx->buf) {
    fs_ctx_abort(ctx);
    lua_pushnil(L);
    lua_pushstring(L, "fs.write out of memory");
    return 2;
  }
  memcpy(ctx->buf, data, len);

  uv_buf_t buf = uv_buf_init(ctx->buf, (unsigned int)len);
  int rc = uv_fs_write(uv_default_loop(), &ctx->req, fd, &buf, 1, -1, lunet_fs_write_cb);
  if (rc < 0) {
    fs_ctx_abort(ctx);
    lua_pushnil(L);
    lua_pushstring(L, uv_strerror(rc));
    return 2;
  }

  return lua_yield(L, 0);
}

static const char *dirent_type_to_string(uv_dirent_type_t type) {
  switch (type) {
    case UV_DIRENT_FILE:   return "file";
    case UV_DIRENT_DIR:    return "dir";
    case UV_DIRENT_LINK:   return "link";
    case UV_DIRENT_FIFO:   return "fifo";
    case UV_DIRENT_SOCKET: return "socket";
    case UV_DIRENT_CHAR:   return "char";
    case UV_DIRENT_BLOCK:  return "block";
    default:               return "unknown";
  }
}

static void lunet_fs_scandir_cb(uv_fs_t *req) {
  fs_ctx_t *ctx = (fs_ctx_t *)req->data;
  lua_State *co = fs_get_co(ctx->L, ctx->co_ref, "fs.scandir");

  if (co) {
    if (req->result < 0) {
      lua_pushnil(co);
      lua_pushstring(co, uv_strerror((int)req->result));
    } else {
      lua_newtable(co);
      int i = 1;
      int r;
      uv_dirent_t ent;

      while ((r = uv_fs_scandir_next(req, &ent)) == 0) {
        lua_newtable(co);
        lua_pushstring(co, ent.name);
        lua_setfield(co, -2, "name");
        lua_pushstring(co, dirent_type_to_string(ent.type));
        lua_setfield(co, -2, "type");
        lua_rawseti(co, -2, i++);
      }

      if (r == UV_EOF) {
        lua_pushnil(co);
      } else {
        lua_pop(co, 1);
        lua_pushnil(co);
        lua_pushstring(co, uv_strerror(r));
      }
    }
    fs_resume(co, 2, "fs.scandir");
  }

  luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->co_ref);
  fs_ctx_free(ctx);
}

int lunet_fs_scandir(lua_State *L) {
  if (lunet_ensure_coroutine(L, "fs.scandir") != 0) {
    return lua_error(L);
  }
  if (lua_gettop(L) < 1 || !lua_isstring(L, 1)) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.scandir requires path");
    return 2;
  }

  const char *path = luaL_checkstring(L, 1);

  fs_ctx_t *ctx = fs_ctx_new(L);
  if (!ctx) {
    lua_pushnil(L);
    lua_pushstring(L, "fs.scandir out of memory");
    return 2;
  }

  int rc = uv_fs_scandir(uv_default_loop(), &ctx->req, path, 0, lunet_fs_scandir_cb);
  if (rc < 0) {
    fs_ctx_abort(ctx);
    lua_pushnil(L);
    lua_pushstring(L, uv_strerror(rc));
    return 2;
  }

  return lua_yield(L, 0);
}