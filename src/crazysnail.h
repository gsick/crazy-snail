
#ifndef __CRAZYSNAIL_H
#define __CRAZYSNAIL_H
#include <lua.h>
#include <uv.h>
#include <stdbool.h>

#include "cb.h"
#include "hiredis-light.h"

#define SNAIL_ERR -1
#define SNAIL_OK 0

/* State of stream */
#define STREAM_CONNECTED 0x1
#define SUB_STREAM_CONNECTED 0x2
/* State of context */
#define CONTEXT_CONNECTED 0x4


/* Context for a connection to Redis */
typedef struct client_context_s {
  /* Unix Domain Socket path */
  char* path;
  bool ignore_sub_reply;
  /* UV_STREAM */
  uv_stream_t* stream;
  uv_stream_t* sub_stream;
  /* LUA State */
  lua_State *L;

  /* Connect Callback */
  int r_connect_cb;
  /* Error Callback */
  int r_error_cb;
  /* Disconnect Callback */
  int r_disconnect_cb;
  /* List of Command Callback */
  callback_ends_t* command_cb_list;
  
  /* Tree of Subscription Callback */
  node_t *channels;
  node_t *patterns;
  node_t *timers;
  
  /* Flags */
  int flags;
  int stream_flags;
  /* Redis Protocol Reader */
  redisReader *reader;  
} client_context_t;

/* Request allocator */
typedef struct req_list_s {
  union uv_any_req uv_req;
  struct req_list_s* next;
} req_list_t;

/* Buffer allocator */
typedef struct buf_list_s {
  uv_buf_t uv_buf_t;
  struct buf_list_s* next;
} buf_list_t;

void stop_timer(uv_timer_t* req);

#endif
