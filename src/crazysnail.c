/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 gsick
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Some part of code are largely inspired from Hiredis (https://github.com/redis/hiredis)
 * and luvit-redis (https://github.com/tadeuszwojcik/luvit-redis)
 */

#include <assert.h>
#include <ctype.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <math.h>
#include <uv.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cb.h"
#include "crazysnail.h"
#include "hiredis-light.h"
#include "luv_handle.h"
#include "sds.h"

#define LUA_CLIENT_MT "lua.crazy.snail.client"
#define LUA_MAX_STACK (LUAI_MAXCSTACK)

#define KEY_EVENT "__keyevent@0__:"
#define KEY_SPACE "__keyspace@0__:"
#define TIMER_EVENT "__timer@0__:"

#define NB_EVENTS 35

static char *events[] = {
	"append", "del", 
	"expire", "evicted",
	"incrby", "incrbyfloat", 
	"hdel", "hincrby", "hincrbyfloat", "hset",
	"linsert", "lpop", "lpush", "lset", "ltrim",
	"rename_from", "rename_to", "rpop", "rpush",
	"sadd", "sdiffstore", "set", "setrange", 
  "sinterstore", "sortstore", "spop", "srem", "sunionostore",
	"zadd", "zincr", "zinterstore", "zrem", "zrembyrank", 
  "zrembyscore", "zunionstore"};

static req_list_t* req_freelist = NULL;
static buf_list_t* buf_freelist = NULL;

static uv_buf_t buf_alloc(uv_handle_t* handle, size_t size);
static void buf_free(const uv_buf_t* buf);
static uv_req_t* req_alloc(void);
static void req_free(uv_req_t* uv_req);

static void stackDump(lua_State *L);

static void on_disconnect(uv_handle_t* handle);
static int push_reply(lua_State *L, redisReply *redisReply);
static int push_sub_reply(lua_State *L, redisReply *redisReply);
static void on_timer(uv_timer_t* handle, int status);

static int get_and_call_sub_cb(client_context_t* cc, redisReply *reply) {

  callback_ends_t* cb_list = NULL;
  node_t* leaf = NULL;
  node_t *callbacks;
  bool pvariant;
  char *stype;
  sds sname;
 
  /* Custom reply functions are not supported for pub/sub. This will fail
   * very hard when they are used... */
  if (reply->type == REDIS_REPLY_ARRAY) {
    assert(reply->elements >= 2);
    assert(reply->element[0]->type == REDIS_REPLY_STRING);
    stype = reply->element[0]->str;
    pvariant = (tolower(stype[0]) == 'p');

    if (pvariant)
      callbacks = cc->patterns;
    else
      callbacks = cc->channels;

    int unsub = strcmp(pvariant ? stype + 1 : stype, "unsubscribe");
    if(unsub == 0)
      return; // for now

    /* Locate the right list callback */
    assert(reply->element[1]->type == REDIS_REPLY_STRING);
    sname = sdsnewlen(reply->element[1]->str,reply->element[1]->len);
    search(sname, callbacks, &cb_list);
        
    /* Set flags */
    callback_ll_t *temp = cb_list->head;
    assert(temp != NULL);

    int init = strcmp(pvariant ? stype + 1 : stype, "subscribe");
    if (init == 0) {
      int done = 0;
       /* Find the right callback to call 
        * It is a sub ok reply, we don't want call all callback */
      while (done == 0 && temp != NULL) {
		    callback_t *cb = temp->cb;
		    if (!(cb->flags & CALLBACK_INITIALIZED)) {
			    int i, all;
			    all = CHANNEL_SUBSCRIBED;
	        /* Find the right channel */		  
          for (i = 0; i <= cb->nb_channel - 1; i++) {
            channel_t *ch = cb->channels[i];
            if (!(ch->flags & CHANNEL_SUBSCRIBED)) {
              if (done == 0 && !(ch->flags & CHANNEL_TIMER_EVENT) 
                  && strcmp(ch->name, sname) == 0) {
                /* Set initialized */
			          ch->flags |= CHANNEL_SUBSCRIBED;
				        done = 1;
			        } else if ((ch->flags & CHANNEL_TIMER_EVENT)) {
                /* Start timer */
                search_timer(ch->ikey, cc->timers, &leaf);
                uv_timer_t* timer_req = (uv_timer_t*)req_alloc();
                timer_req->data = cc;
                uv_timer_init(uv_default_loop(), timer_req);
                uv_timer_start(timer_req, on_timer, 0, ch->ikey);
                ch->flags |= CHANNEL_SUBSCRIBED;
                leaf->data = timer_req;
              }
            }
            
			      all &= ch->flags;
          }
          /* If all channels are initialized, 
           * the callback is initialized */
          if(all & CHANNEL_SUBSCRIBED) {
		        cb->flags |= CALLBACK_INITIALIZED;
			    }
          /* Call Callback */
			    if (!cc->ignore_sub_cmd_reply && done == 1) {
			      lua_State *L = cc->L;
            lua_rawgeti(L, LUA_REGISTRYINDEX, temp->cb->ref);
            
            lua_pushnil(L);
            int argc = push_sub_reply(L, reply);
            lua_pcall(cc->L, argc + 1, 0, 0);
			    }
        }
        temp = temp->next;
      }
    } else {
     while(temp != NULL) {
			
        lua_State *L = cc->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, temp->cb->ref);
            
        if (!(temp->cb->flags & CALLBACK_INITIALIZED)) {
			    lua_pushstring(cc->L, "event received but not initialized");
			    lua_pcall(cc->L, 1, 0, 0);
			  } else {
          lua_pushnil(L);
          int argc = push_sub_reply(L, reply);
          lua_pcall(cc->L, argc + 1, 0, 0);
        }
        temp = temp->next;
     }
   }

        
        /*
        if (de != NULL) {
            memcpy(dstcb,dictGetEntryVal(de),sizeof(*dstcb));

            /* If this is an unsubscribe message, remove it. *
            if (strcasecmp(stype+pvariant,"unsubscribe") == 0) {
                dictDelete(callbacks,sname);

                /* If this was the last unsubscribe message, revert to
                 * non-subscribe mode. *
                assert(reply->element[2]->type == REDIS_REPLY_INTEGER);
                if (reply->element[2]->integer == 0)
                    c->flags &= ~REDIS_SUBSCRIBED;
            }
        }
        */
    sdsfree(sname);
  }

  return REDIS_OK;
}

static int push_sub_reply(lua_State *L, redisReply *redisReply) {

  bool tweak = true;

  switch(redisReply->type) {
    case REDIS_REPLY_ERROR:
      luv_push_async_error_raw(L, NULL, redisReply->str, "push_reply", NULL);
      break;

    case REDIS_REPLY_STATUS:
      lua_pushlstring(L, redisReply->str, redisReply->len);
      break;

    case REDIS_REPLY_INTEGER:
      lua_pushinteger(L, redisReply->integer);
      break;

    case REDIS_REPLY_NIL:
      lua_pushnil(L);
      break;

    case REDIS_REPLY_STRING: {
      
      if (!tweak) {
        lua_pushlstring(L, redisReply->str, redisReply->len);
        break;
      }
        int key_space_prefix_len = strlen(KEY_SPACE);
        int key_event_prefix_len = strlen(KEY_EVENT);
        
        if (strncmp(redisReply->str, KEY_SPACE, key_space_prefix_len) == 0) {
          lua_pushlstring(L, redisReply->str + key_space_prefix_len, redisReply->len - key_space_prefix_len);
        } else if (strncmp(redisReply->str, KEY_EVENT, key_event_prefix_len) == 0) {
          lua_pushlstring(L, redisReply->str + key_event_prefix_len, redisReply->len - key_event_prefix_len);
        } else {
          lua_pushlstring(L, redisReply->str, redisReply->len);
        }
      break;
    }

    case REDIS_REPLY_ARRAY: {
      unsigned int i;
      lua_createtable(L, redisReply->elements, 0);

      for (i = tweak ? 1 : 0; i < redisReply->elements; ++i) {
        push_sub_reply(L, redisReply->element[i]);
        lua_rawseti(L, -2, i + 1); /* Store sub-reply */
      }

      break;
    }

    default:
      return luaL_error(L, "Unknown reply type: %d", redisReply->type);
  }

  return 1;
}

static int push_reply(lua_State *L, redisReply *redisReply) {

  switch(redisReply->type) {
    case REDIS_REPLY_ERROR:
      luv_push_async_error_raw(L, NULL, redisReply->str, "push_reply", NULL);
      break;

    case REDIS_REPLY_STATUS:
      lua_pushlstring(L, redisReply->str, redisReply->len);
      break;

    case REDIS_REPLY_INTEGER:
      lua_pushinteger(L, redisReply->integer);
      break;

    case REDIS_REPLY_NIL:
      lua_pushnil(L);
      break;

    case REDIS_REPLY_STRING:
      lua_pushlstring(L, redisReply->str, redisReply->len);
      break;

    case REDIS_REPLY_ARRAY: {
      unsigned int i;
      lua_createtable(L, redisReply->elements, 0);

      for (i = 0; i < redisReply->elements; ++i) {
        push_reply(L, redisReply->element[i]);
        lua_rawseti(L, -2, i + 1); /* Store sub-reply */
      }

      break;
    }

    default:
      return luaL_error(L, "Unknown reply type: %d", redisReply->type);
  }

  return 1;
}


static void on_timer(uv_timer_t* handle, int status) {

  client_context_t* cc = (client_context_t*)handle->data;
  
  if (cc->flags & REDIS_DISCONNECTING) {
    return;
  }

  node_t* leaf = NULL;

  search_timer(handle->repeat, cc->timers, &leaf);

  callback_ll_t *temp = leaf->cb_list->head;
  while(temp != NULL) {

    lua_State *L = cc->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, temp->cb->ref);

    if (!(temp->cb->flags & CALLBACK_INITIALIZED)) {
      lua_pushstring(L, "event received but not initialized");
      lua_pcall(L, 1, 0, 0);
    } else {
      lua_pushnil(L);
      lua_createtable(L, 3, 0);
      lua_pushstring(L, "timer");
      lua_rawseti(L, -2, 1);
      lua_pushinteger(L, handle->repeat);
      lua_rawseti(L, -2, 2);
      lua_pushinteger(L, handle->timeout);
      lua_rawseti(L, -2, 3);
      lua_pcall(L, 2, 0, 0);
    }
    temp = temp->next;
  }
}


static void on_read(uv_stream_t* stream, ssize_t nread, uv_buf_t buf) {

  client_context_t* cc = (client_context_t*)stream->data;
  bool sub_mode = (cc->sub_stream == stream);

if (cc->flags & REDIS_DISCONNECTING) {
  buf_free(&buf);
  return;
}

  /* Error or connection closed by server */
  if (nread < 0) {
    /* Call Error Callback */
    if (cc->r_error_cb != LUA_NOREF && cc->r_error_cb != LUA_REFNIL) {
      const char* error = uv_strerror(uv_last_error(uv_default_loop()));
      lua_rawgeti(cc->L, LUA_REGISTRYINDEX, cc->r_error_cb);
      lua_pushstring(cc->L, error);
      lua_pcall(cc->L, 1, 0, 0);
    }
    /* Disconnect */
    uv_close((uv_handle_t*)stream, on_disconnect);
    return;
  }

  if (nread > 0) {
    if (redisReaderFeed(cc->reader,buf.base,nread) != REDIS_OK) {
      /* Call Error Callback */
      if (cc->r_error_cb != LUA_NOREF && cc->r_error_cb != LUA_REFNIL) {
        lua_rawgeti(cc->L, LUA_REGISTRYINDEX, cc->r_error_cb);
        lua_pushstring(cc->L, cc->reader->errstr);
        lua_pcall(cc->L, 1, 0, 0);
      }
      buf_free(&buf);
      return;
    }

    buf_free(&buf);

    callback_t cb;
    void *reply = NULL;
    int status;
    while ((status = redisReaderGetReply(cc->reader,&reply)) == REDIS_OK) {
      if (reply == NULL) {
        /* When the connection is being disconnected and there are
         * no more replies, this is the cue to really disconnect. */
        if (cc->flags & REDIS_DISCONNECTING) {
          //__redisAsyncDisconnect(ac);
          return;
        }

        /* If monitor mode, repush callback */
        if (cc->flags & REDIS_MONITORING) {
          callback_ll_t* wrapper = NULL;
          if ( wrap_cb(&wrapper, &cb) == 0 ) {
            push_cb(&cc->command_cb_list, wrapper);
          }
        }

        /* When the connection is not being disconnected, simply stop
         * trying to get replies and wait for the next loop tick. */
        break;
      }
      
      if (sub_mode) {
	      if (((redisReply*)reply)->type == REDIS_REPLY_ERROR) {
		      // disconnect??
		    } else {
		      get_and_call_sub_cb(cc, reply);
	      }
        cc->reader->fn->freeObject(reply);
      } else {
	      if (shift_cb(&cc->command_cb_list, &cb) != 0) {
		      if (((redisReply*)reply)->type == REDIS_REPLY_ERROR) {
		        // disconnect??
		      }
	      }

	      if (cb.ref != LUA_NOREF && cb.ref != LUA_REFNIL) {
	        lua_State *L = cc->L;
          lua_rawgeti(L, LUA_REGISTRYINDEX, cb.ref);
          luaL_unref(L, LUA_REGISTRYINDEX, cb.ref);
    
          lua_pushnil(L);
          int argc = push_reply(L, reply);
          lua_pcall(L, argc + 1, 0, 0);
  
          cc->reader->fn->freeObject(reply);
		    } else {
          /* No callback for this reply. This can either be a NULL callback,
           * or there were no callbacks to begin with. Either way, don't
           * abort with an error, but simply ignore it because the client
           * doesn't know what the server will spit out over the wire. */
           cc->reader->fn->freeObject(reply);
        }  
	    }
    }
    
    if (reply != NULL) {
      cc->reader->fn->freeObject(reply);
    }
    if (status == REDIS_ERR) {
      //TODO disconnect?
      return;
    }
  }

  /* Not subscribed context or No more callback */
  if (!sub_mode 
      && cc->command_cb_list->head == NULL) {
    uv_read_stop(stream);
  }
}


static void on_write(uv_write_t* handle, int status) {

  client_context_t* cc = (client_context_t*)handle->data;
  uv_stream_t* stream = handle->handle;
  stream->data = cc;

  req_free((uv_req_t*)handle);

  if (status < 0) {
    /* Call Callback */
    callback_t cb;
    if (shift_cb(&cc->command_cb_list, &cb) == 0) {
      if (cb.ref != LUA_NOREF && cb.ref != LUA_REFNIL) {
        const char* error = uv_strerror(uv_last_error(uv_default_loop()));
        lua_rawgeti(cc->L, LUA_REGISTRYINDEX, cb.ref);
        luaL_unref(cc->L, LUA_REGISTRYINDEX, cb.ref);
        lua_pushstring(cc->L, error);
        lua_pcall(cc->L, 1, 0, 0);
      }
    }
    return;
  }
  assert(status == 0);

  /* Start Reading */
  int r = uv_read_start(stream, buf_alloc, on_read);
  if (r < 0) {
    /* Call Callback */
    const char* error = uv_strerror(uv_last_error(uv_default_loop()));
    // TODO not the right cb in substream, false in sub stream
    callback_t cb;
    if ( shift_cb(&cc->command_cb_list, &cb) == 0 ) {
       
      if (cb.ref != LUA_NOREF && cb.ref != LUA_REFNIL) {
        lua_rawgeti(cc->L, LUA_REGISTRYINDEX, cb.ref);
        luaL_unref(cc->L, LUA_REGISTRYINDEX, cb.ref);

        lua_pushstring(cc->L, error);
        lua_pcall(cc->L, 1, 0, 0);
      }
    }
  }

  return;
}


static void on_connect(uv_connect_t* handle, int status) {

  client_context_t* cc = (client_context_t*)handle->data;
  req_free((uv_req_t*)handle);

  if (status < 0) {
    /* Call Error Callback */
    if (cc->r_error_cb != LUA_NOREF && cc->r_error_cb != LUA_REFNIL) {
      const char* error = uv_strerror(uv_last_error(uv_default_loop()));
      lua_rawgeti(cc->L, LUA_REGISTRYINDEX, cc->r_error_cb);
      lua_pushstring(cc->L, error);
      lua_pcall(cc->L, 1, 0, 0);
      return;
    }

    /* Disconnect */
    uv_close((uv_handle_t*)cc->stream, on_disconnect);
    return;
  }
  assert(status == 0);
  
  if (!(cc->stream_flags & STREAM_CONNECTED)) {
    cc->stream_flags |= STREAM_CONNECTED;
  } else if ((cc->stream_flags & STREAM_CONNECTED) 
      && !(cc->stream_flags & SUB_STREAM_CONNECTED)) {
    cc->stream_flags |= SUB_STREAM_CONNECTED;
  }
  
  if ((cc->stream_flags & STREAM_CONNECTED) 
      && (cc->stream_flags & SUB_STREAM_CONNECTED)) {
    cc->flags |= REDIS_CONNECTED;
  
    /* Call Connect Callback */
    if (cc->r_connect_cb != LUA_NOREF && cc->r_connect_cb != LUA_REFNIL) {
      lua_rawgeti(cc->L, LUA_REGISTRYINDEX, cc->r_connect_cb);
      lua_pcall(cc->L, 0, 0, 0);
    }
  }
  return;
}


static int lua_client_command(lua_State *L) {
#ifdef LUA_STACK_CHECK
  //stackDump(L);
  int top = lua_gettop(L);
#endif
  static const char *argv[LUA_MAX_STACK];
  static size_t argvlen[LUA_MAX_STACK];
  static const char *timers[100];

  client_context_t *cc = (client_context_t*)
                           luaL_checkudata(L, 1, LUA_CLIENT_MT);

  int argc, i, nb_timers;
  argc = 0;
  nb_timers = 0;
  /* Is there callback? */
  int ltop = lua_isfunction(L, -1) ? lua_gettop(L) -1 : lua_gettop(L);
  
  /* Redis cmd */
  for (i = 2; i <= ltop; i++) {
    if (lua_istable(L, i)) {
      int j;
      int length = lua_objlen(L, i);
      for (j = 0; j < length; j++) {
        lua_rawgeti(L, i, j + 1);
        argv[argc] = lua_tolstring(L, -1, &argvlen[argc]);
        lua_pop(L, 1);
        
        if (argv[argc] == NULL) {
          return luaL_argerror(L, i, "command: Not a string or a number");
        }
        
        if (++argc > LUA_MAX_STACK - 1) {
          return luaL_error(L, "command: Stack Overflow");
        }
      }
    } else {
      size_t key_s;
      const char * key = lua_tolstring(L, i, &key_s);
      /* Remove timer key */
      if (strncmp(key, TIMER_EVENT, strlen(TIMER_EVENT)) != 0) {
        argv[argc] = key;
        argvlen[argc] = key_s;
        if (argv[argc] == NULL) {
          return luaL_argerror(L, i, "command: Not a string or a number");
        }

        if (++argc > LUA_MAX_STACK - 1) {
          return luaL_error(L, "command: Stack Overflow");
        }
      } else {
        timers[nb_timers++] = key;
      }
    }
  }
  
  char *cmd;
  int len;
  len = redisFormatCommandArgv(&cmd,argc,argv,argvlen);

  /* Callback */
  callback_t *cb = NULL;
  int ref = LUA_REFNIL;
  if (lua_isfunction(L, -1)) {
    ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  int pvariant = (tolower(argv[0][0]) == 'p') ? 1 : 0;
  bool sub_mode = false;

  if (strncasecmp(argv[0] + pvariant,"subscribe",9) == 0) {
	  sub_mode = true;
    
    /* Create callback with channels */
    if (create_callback(&cb, ref, argc - 1 + nb_timers) == 0) {
      /* Add every channel/pattern to the list of subscription callbacks. */
      int k;
      for (k = 1; k <= argc-1; k++) {
	      /* Create channel */
	      channel_t *ch = NULL;
        if (create_channel(&ch, argv[k]) == 0) {
          cb->channels[k-1] = ch;
	      }
        /* Add to list/tree */
        node_t *leaf = NULL;
        if (pvariant) {
          insert(&(cc->patterns), &leaf, argv[k]);
        } else {
          insert(&(cc->channels), &leaf, argv[k]);
        }

        callback_ll_t* wrapper = NULL;
        if (wrap_cb(&wrapper, cb) == 0) {
          push_cb(&(leaf->cb_list), wrapper);
	      }
      }
      for (k = 0; k <= nb_timers -1; k++) {
        const char *key = timers[k];
        uint64_t ikey = strtol(key + strlen(TIMER_EVENT), (char **)NULL, 10);
        /* Create channel */
	      channel_t *ch = NULL;
        if (create_timer_channel(&ch, ikey) == 0) {
          cb->channels[argc - 1 + k] = ch;
	      }
        
        int status;
        /* Add to list/tree */
        node_t *leaf = NULL;
        if ( (status = insert_timer(&(cc->timers), &leaf, ikey)) >= 1) {
          if (status != 2) {
            ch->flags |= CHANNEL_SUBSCRIBED;
          }
          
          callback_ll_t* wrapper = NULL;
          if (wrap_cb(&wrapper, cb) == 0) {
            push_cb(&(leaf->cb_list), wrapper);
	        }
        }
      }
    }
  } else if (strncasecmp(argv[0] + pvariant,"unsubscribe",11) == 0) {
    sub_mode = true;
    /* It is only useful to call (P)UNSUBSCRIBE when the context is
    * subscribed to one or more channels or patterns. */
    if (!(cc->flags & REDIS_SUBSCRIBED)) {
      //return REDIS_ERR;
    }
    /* (P)UNSUBSCRIBE does not have its own response: every channel or
     * pattern that is unsubscribed will receive a message. This means we
    * should not append a callback function for this command. */
  } else {
    
    if (strncasecmp(argv[0],"monitor",7) == 0) {
      /* Set monitor flag and push callback */
      cc->flags |= REDIS_MONITORING;
    }
    
    callback_ll_t* wrapper = NULL;
    if ( create_callback(&cb, ref, 0) == 0 
      && wrap_cb(&wrapper, cb) == 0 ) {
      push_cb(&cc->command_cb_list, wrapper);
    }
  }

  /* Start Writing */
  int r = 0;
  uv_write_t* req = NULL;
  if (cc->flags & REDIS_CONNECTED) {
    uv_buf_t buf = uv_buf_init(cmd, len);
    req = (uv_write_t*)req_alloc();
    req->data = cc;
    r = uv_write(req, sub_mode ? cc->sub_stream : cc->stream, 
        &buf, 1, on_write);
  }
  if (r == 0) {
    free(cmd);
  }

  /* Error */
  if (!(cc->flags & REDIS_CONNECTED) 
    || (cc->flags & (REDIS_DISCONNECTING | REDIS_FREEING)) 
    || r < 0) {

   if (req != NULL ) {
     req_free((uv_req_t*)req);
   }

   const char* error = r < 0 ? 
				  uv_strerror(uv_last_error(uv_default_loop())) 
				  : "command: Not connected";

    /* Unref and call the callback (if there is) with error */	
    callback_t cb;
    if(shift_cb(&cc->command_cb_list, &cb) == 0) {
      if (cb.ref != LUA_NOREF && cb.ref != LUA_REFNIL) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, cb.ref);
        luaL_unref(L, LUA_REGISTRYINDEX, cb.ref);

        lua_pushstring(L, error);
        lua_pcall(L, 1, 0, 0);
        return 0;
      }
    } else if (sub_mode) {
      // TODO: crash badly
    }

#ifdef LUA_STACK_CHECK
    assert(lua_gettop(L) == top - 1);
#endif
    /* If no error callback, print stack trace */
    return luaL_error(L, error);
  }
  assert(r == 0);

  lua_pushvalue(L, 1);
#ifdef LUA_STACK_CHECK
  assert(lua_gettop(L) == top);
#endif
  return 1;
}


static int lua_client_subscribe(lua_State *L) {
#ifdef LUA_STACK_CHECK
  int vtop = lua_gettop(L);
#endif

  int top = lua_gettop(L);
  
  int key_space_prefix_len = strlen(KEY_SPACE);
  int key_event_prefix_len = strlen(KEY_EVENT);
  int timer_event_prefix_len = strlen(TIMER_EVENT);
  
  int i;
  for (i = 2; i <= top -1; i++) {
    const char *key;
    char* buffer;
    /* Is it a number */
    bool is_number = false;
    bool add_prefix = false;
    if (lua_isnumber (L, i)) {
      is_number = true;
      add_prefix = true;
      int timeout = luaL_checkint(L, i);
      int len = (timeout == 0 ? 1 : (int)(log10(timeout)+1));
      buffer = malloc((len + 1) * sizeof(char));
      snprintf(buffer, len + 1, "%d", timeout);
      key = buffer;
    } else {
      key = luaL_checkstring(L, i);
      /* Add prefix if needed */
      add_prefix = !(strncmp(key, KEY_SPACE, key_space_prefix_len) == 0
              || strncmp(key, KEY_EVENT, key_event_prefix_len) == 0
              || strncmp(key, TIMER_EVENT, timer_event_prefix_len) == 0);
    }

    if (add_prefix) {
      /* Is it an event ? */
      int j;
      bool event = false;
      for (j = 0; j <= NB_EVENTS - 1; j++) {
	      if (strcmp(events[j], key) == 0) {
		      event = true;
		      break;
	      }
	    }
    
      char *newkey = malloc((strlen(is_number ? TIMER_EVENT : event ? KEY_EVENT : KEY_SPACE) + strlen(key) + 1) * sizeof(char));
      if (newkey == NULL) {
        return luaL_error(L, "subscribe: Out Of Memory");
      }
      strcpy(newkey, is_number ? TIMER_EVENT : event ? KEY_EVENT : KEY_SPACE);
      strcat(newkey, key);
      lua_pushstring(L, newkey);
      lua_replace(L, i);
      free(newkey);
      if (is_number) {
        free(buffer);
      }
    }
  }
  lua_pushstring(L, "subscribe");
  lua_insert(L, 2);
  
#ifdef LUA_STACK_CHECK
  assert(lua_gettop(L) == vtop + 1);
#endif
  return lua_client_command(L);
}


static int lua_client_on(lua_State *L) {
#ifdef LUA_STACK_CHECK
  int top = lua_gettop(L);
#endif
  client_context_t *cc = (client_context_t*)
                           luaL_checkudata(L, 1, LUA_CLIENT_MT);

  const char *event_name = luaL_checkstring(L, 2);
  
  if (lua_isfunction(L, -1)) {

    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    
    if (strcmp(event_name, "error") == 0) {
      cc->r_error_cb = ref;
    } else if (strcmp(event_name, "connect") == 0) {
      cc->r_connect_cb = ref;
    } else if (strcmp(event_name, "disconnect") == 0) {
      cc->r_disconnect_cb = ref;
    } else {
      luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
  }

  lua_pushvalue(L, 1);
#ifdef LUA_STACK_CHECK
  assert(lua_gettop(L) == top);
#endif
  return 1;
}


static int lua_client_connect(lua_State *L) {
#ifdef LUA_STACK_CHECK
  stackDump(L);
  int top = lua_gettop(L);
#endif
  client_context_t *cc = (client_context_t*)
                           luaL_checkudata(L, 1, LUA_CLIENT_MT);

  /* Initialize pipe */
  uv_pipe_t* stream = (uv_pipe_t*)malloc(sizeof(uv_pipe_t));
  if (stream == NULL) {
    return luaL_error(L, "new: Out Of Memory");
  }
  uv_pipe_init(uv_default_loop(), stream, 0);

  uv_pipe_t* sub_stream = (uv_pipe_t*)malloc(sizeof(uv_pipe_t));
  if (sub_stream == NULL) {
    return luaL_error(L, "new: Out Of Memory");
  }
  uv_pipe_init(uv_default_loop(), sub_stream, 0);
  
  cc->stream = (uv_stream_t*)stream;
  cc->sub_stream = (uv_stream_t*)sub_stream;
  cc->flags = 0;//&= ~REDIS_CONNECTED;
//cc->reader = redisReaderCreate();

  uv_connect_t* req = (uv_connect_t*)req_alloc();
  req->data = cc;
  uv_pipe_connect(req, (uv_pipe_t*)cc->stream, cc->path, on_connect);
  
  uv_connect_t* sub_req = (uv_connect_t*)req_alloc();
  sub_req->data = cc;
  uv_pipe_connect(sub_req, (uv_pipe_t*)cc->sub_stream, cc->path, on_connect);

  lua_pushvalue(L, 1);
#ifdef LUA_STACK_CHECK
  assert(lua_gettop(L) == top + 1);
#endif
  return 1;
}


static int lua_client_exit(lua_State *L) {
#ifdef LUA_STACK_CHECK
  int top = lua_gettop(L);
#endif
  client_context_t *cc = (client_context_t*)
                           luaL_checkudata(L, 1, LUA_CLIENT_MT);

  uv_close((uv_handle_t*)cc->stream, NULL);
  uv_close((uv_handle_t*)cc->sub_stream, NULL);
  
  if (cc->r_connect_cb != LUA_NOREF && cc->r_connect_cb != LUA_REFNIL) {
    luaL_unref(cc->L, LUA_REGISTRYINDEX, cc->r_connect_cb);
  }
  if (cc->r_error_cb != LUA_NOREF && cc->r_error_cb != LUA_REFNIL) {
    luaL_unref(cc->L, LUA_REGISTRYINDEX, cc->r_error_cb);
  }
  if (cc->r_disconnect_cb != LUA_NOREF && cc->r_disconnect_cb != LUA_REFNIL) {
    luaL_unref(cc->L, LUA_REGISTRYINDEX, cc->r_disconnect_cb);
  }
  
  cc->stream_flags &= ~STREAM_CONNECTED;
  cc->stream_flags &= ~SUB_STREAM_CONNECTED;
  
  destroy_tree(&cc->channels);
  destroy_tree(&cc->patterns);
  destroy_tree(&cc->timers);
  destroy_list(&cc->command_cb_list);
  
  free(cc->stream);
  free(cc->sub_stream);
  free(cc->path);
  if (cc->reader != NULL)
    redisReaderFree(cc->reader);

#ifdef LUA_STACK_CHECK
  assert(lua_gettop(L) == top);
#endif
  return 0;
}


static void on_disconnect(uv_handle_t* handle) {

  client_context_t* cc = (client_context_t*)handle->data;
  free(handle);

  if (cc->stream_flags & STREAM_CONNECTED) {
    cc->stream_flags &= ~STREAM_CONNECTED;
  } else if (!(cc->stream_flags & STREAM_CONNECTED) 
      && (cc->stream_flags & SUB_STREAM_CONNECTED)) {
	  cc->stream_flags &= ~SUB_STREAM_CONNECTED;
  }

  if ( !(cc->stream_flags & STREAM_CONNECTED) && !(cc->stream_flags & SUB_STREAM_CONNECTED) ) {
    cc->flags |= REDIS_FREEING;

    destroy_tree(&cc->channels);
    destroy_tree(&cc->patterns);
    destroy_tree(&cc->timers);
    destroy_list(&cc->command_cb_list);

    // call disconnect callback
    if (cc->r_disconnect_cb != LUA_NOREF && cc->r_disconnect_cb != LUA_REFNIL) {
      lua_rawgeti(cc->L, LUA_REGISTRYINDEX, cc->r_disconnect_cb);
      lua_pcall(cc->L, 0, 0, 0);
    }
    //if (cc->reader != NULL)
      //redisReaderFree(cc->reader);
  }
}


static int lua_client_disconnect(lua_State *L) {
#ifdef LUA_STACK_CHECK
  int top = lua_gettop(L);
#endif
  client_context_t *cc = (client_context_t*)
                           luaL_checkudata(L, 1, LUA_CLIENT_MT);

  if (!(cc->flags & REDIS_DISCONNECTING)) {

    cc->stream->data = cc;
    cc->sub_stream->data = cc;

    //uv_read_stop(cc->stream);
    uv_close((uv_handle_t*)cc->stream, on_disconnect);

    //uv_read_stop(cc->sub_stream);
    uv_close((uv_handle_t*)cc->sub_stream, on_disconnect);

    cc->flags |= REDIS_DISCONNECTING;
  }

  lua_pushvalue(L, 1);
#ifdef LUA_STACK_CHECK
  assert(lua_gettop(L) == top + 1);
#endif
  return 1;
}


static int lua_client_new(lua_State *L) {
#ifdef LUA_STACK_CHECK
  int top = lua_gettop(L);
#endif
  client_context_t *cc;
  const char *path;
  bool ignore_sub_cmd_reply = true;

  // check if table
  luaL_checktype(L, 1, LUA_TTABLE);
  
  /* Options */
  /* UDS path */
  lua_pushstring(L, "path");
  lua_gettable(L, -2 );
  path = luaL_checkstring(L, -1);
  lua_pop(L,1);
  /* Ignore sub reply */
  lua_pushstring(L, "ignore_sub_cmd_reply");
  lua_gettable(L, -2 );
  if (lua_isboolean(L, -1)) {
    ignore_sub_cmd_reply = lua_toboolean(L, -1);
  }
  lua_pop(L,1);

  /* Initialize Context */
  cc = (client_context_t*)
         lua_newuserdata(L, sizeof(client_context_t));
  cc->path = strdup(path);
  cc->ignore_sub_cmd_reply = ignore_sub_cmd_reply;
  cc->stream = NULL;
  cc->sub_stream = NULL;
  cc->L = L;
  cc->r_connect_cb = LUA_NOREF;
  cc->r_disconnect_cb = LUA_NOREF;
  cc->r_error_cb = LUA_NOREF;
  cc->flags = 0;
  cc->stream_flags = 0;
  cc->reader = redisReaderCreate();
  cc->channels = NULL;
  cc->patterns = NULL;
  cc->timers = NULL;
  cc->command_cb_list = (callback_ends_t*)malloc(sizeof(callback_ends_t));
  cc->command_cb_list->head = NULL;
  cc->command_cb_list->tail = NULL;
  
  luaL_getmetatable(L, LUA_CLIENT_MT);
  lua_setmetatable(L, -2);

  lua_newtable(L);
  lua_setfenv (L, -2);
#ifdef LUA_STACK_CHECK
  assert(lua_gettop(L) == top + 1);
#endif
  return 1;
}


static const struct luaL_Reg functions[] = {
  {"new", lua_client_new},
  {NULL, NULL}
};


static const struct luaL_Reg methods[] = {
  {"on", lua_client_on},
  {"connect", lua_client_connect},
  {"disconnect", lua_client_disconnect},
  {"exit", lua_client_exit},
  {"subscribe", lua_client_subscribe},
  {"command", lua_client_command},
  {NULL, NULL}
};


int luaopen_crazysnail(lua_State *L) {

  luaL_newmetatable(L, LUA_CLIENT_MT);
  luaL_register(L, NULL, methods);
  luaL_register(L, NULL, functions);
  //lua_pop(L, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  
  //lua_newtable(L);
  //luaL_register(L, NULL, functions);
  return 1;
}

void stop_timer(uv_timer_t* req) {
  uv_timer_stop(req);
  req_free((uv_req_t*)req);
}

//static void buf_alloc(uv_handle_t* handle, size_t size, uv_buf_t* buf) {
static uv_buf_t buf_alloc(uv_handle_t* handle, size_t size) {
  buf_list_t* ab;

  ab = buf_freelist;
  if (ab != NULL) {
    buf_freelist = ab->next;
  } else {
    ab = malloc(size + sizeof(*ab));
    ab->uv_buf_t.len = size;
    ab->uv_buf_t.base = (char*) (ab + 1);
  }

  //*buf = ab->uv_buf_t;
  return ab->uv_buf_t;
}


static void buf_free(const uv_buf_t* buf) {
  buf_list_t* ab = (buf_list_t*) buf->base - 1;
  ab->next = buf_freelist;
  buf_freelist = ab;
}


static uv_req_t* req_alloc(void) {
  req_list_t* req;

  req = req_freelist;
  if (req != NULL) {
    req_freelist = req->next;
  } else {
    req = (req_list_t*) malloc(sizeof *req);
  }

  return (uv_req_t*) req;
}


static void req_free(uv_req_t* uv_req) {
  req_list_t* req = (req_list_t*) uv_req;

  req->next = req_freelist;
  req_freelist = req;
}


static void stackDump(lua_State *L) {
  int i;
  int top = lua_gettop(L);
  for (i = 1; i <= top; i++) {  /* repeat for each level */
    int t = lua_type(L, i);
    switch (t) {
      case LUA_TSTRING:  /* strings */
        printf("`%s'", lua_tostring(L, i));
        break;
    
      case LUA_TBOOLEAN:  /* booleans */
        printf(lua_toboolean(L, i) ? "true" : "false");
        break;
    
      case LUA_TNUMBER:  /* numbers */
        printf("%g", lua_tonumber(L, i));
        break;
    
      default:  /* other values */
        printf("%s", lua_typename(L, t));
        break;
    }
    printf("  ");  /* put a separator */
  }
  printf("\n");  /* end the listing */
}
