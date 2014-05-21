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

#ifndef __CB_H
#define __CB_H

#include <uv.h>

/* State of the channel */
#define CHANNEL_SUBSCRIBED 0x1
/* Is it key space subscription? */
#define CHANNEL_KEY_SPACE 0x2
/* Is it key event subscription? */
#define CHANNEL_KEY_EVENT 0x4
/* Is it timer event subscription? */
#define CHANNEL_TIMER_EVENT 0x8

/* State of the callback */
#define CALLBACK_INITIALIZED 0x1

/* Channel type */
typedef struct channel_s {
  char* name;
  uint64_t ikey;
  int flags;
} channel_t;

/* Callback type */
typedef struct callback_s {
  /* LUA callback function ref */
  int ref;
  int flags;
  int nb_channel;
  int attach;
  channel_t **channels;
} callback_t;

/* Simple linked list */
typedef struct callback_ll_s {
  callback_t *cb;
  struct callback_ll_s *next;
} callback_ll_t;

/* List of callbacks for either regular replies or pub/sub */
typedef struct callback_ends_s {
  callback_ll_t *head, *tail;
} callback_ends_t;

/* Tree node */
typedef struct node_s {
  char* key;
  uint64_t ikey;
  struct node_s *left, *right;
  /* List of Callback */
  callback_ends_t* cb_list;
  /* Timer */
  void* data;
} node_t;

int create_callback(callback_t** callback, int ref, int nb_channel);
void destroy_callback(callback_t* callback);
int create_channel(channel_t** channel, const char* name);
void destroy_channel(channel_t* channel);
int create_timer_channel(channel_t** channel, uint64_t ikey);

int insert(node_t **root, node_t **leaf, const char* key);
void destroy_tree(node_t **root);
void search(const char* key, node_t *leaf, callback_ends_t** cb_list);

int insert_timer(node_t **root, node_t **leaf, uint64_t key);
void search_timer(uint64_t key, node_t *root, node_t** leaf);

void destroy_list(callback_ends_t **cb_list);

void dump_tree(node_t* node);
void dump_list(callback_ends_t* cb_list);

int wrap_cb(callback_ll_t** wrapper, callback_t* cb);
void push_cb(callback_ends_t** list, callback_ll_t* source);
int shift_cb(callback_ends_t** list, callback_t* target);

#endif
