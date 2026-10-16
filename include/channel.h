#ifndef LUNET_CHANNEL_H
#define LUNET_CHANNEL_H

#include <stddef.h>
#include <lua.h>

typedef enum {
  MSG_NIL,
  MSG_BOOL,
  MSG_INTEGER,
  MSG_NUMBER,
  MSG_STRING
} msg_type_t;

typedef struct msg_s {
  msg_type_t type;
  union {
    int b;
    lua_Integer i;
    lua_Number n;
    struct {
      char *data;
      size_t len;
    } s;
  } v;
} msg_t;

msg_t *lunet_msg_from_lua(lua_State *L, int idx);
void lunet_msg_to_lua(lua_State *L, const msg_t *m);
void lunet_msg_free(msg_t *m);

int lunet_channel_create(lua_State *L);
int lunet_channel_close(lua_State *L);
int lunet_channel_clear(lua_State *L);
int lunet_channel_push(lua_State *L);
int lunet_channel_pop(lua_State *L);
int lunet_channel_demand(lua_State *L);  // только внутри корутины, не блокирует loop
int lunet_channel_supply(lua_State *L);  // только внутри корутины, ждёт получения

#endif