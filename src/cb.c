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

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "cb.h"
#include "crazysnail.h"

int wrap_cb(callback_ll_t** wrapper, callback_t* cb) {
  assert(*wrapper == NULL);
  assert(cb != NULL);

  *wrapper = (callback_ll_t*)malloc(sizeof(callback_ll_t));
  if (*wrapper == NULL) {
    return SNAIL_ERR;
  }
  
  (*wrapper)->cb = cb;
  (*wrapper)->next = NULL;

  return SNAIL_OK;
}


void destroy_wrapper(callback_ll_t** wrapper) {
  assert(*wrapper != NULL);

  if ((*wrapper)->cb->attach == 1) {
    destroy_callback((*wrapper)->cb);
  } else {
    (*wrapper)->cb->attach--;
  }

  free(*wrapper);
  *wrapper = NULL;
}


void push_cb(callback_ends_t** list, callback_ll_t* source) {
  assert(*list != NULL);
  assert(source != NULL);

  /* Store callback in list */
  if ((*list)->head == NULL)
    (*list)->head = source;
  if ((*list)->tail != NULL)
    (*list)->tail->next = source;
  (*list)->tail = source;

  source->cb->attach++;
}


int shift_cb(callback_ends_t** list, callback_t* target) {

  callback_ll_t *cb = (*list)->head;
  if (cb != NULL) {
    (*list)->head = cb->next;

    if (cb == (*list)->tail) {
      (*list)->tail = NULL;
    }
    /* Copy callback from heap to stack */
    if (target != NULL) {
      memcpy(target,cb->cb,sizeof(callback_t));
    }

    destroy_wrapper(&cb);

    return SNAIL_OK;
  }
  return SNAIL_ERR;
}


int create_callback(callback_t** callback, int ref, int nb_channel) {
  assert(*callback == NULL);
  
  *callback = (callback_t*)malloc(sizeof(callback_t));
  if (*callback == NULL) {
    return SNAIL_ERR;
  }
  (*callback)->ref = ref;
  (*callback)->nb_channel = nb_channel;
  (*callback)->attach = 0;
  (*callback)->channels = (channel_t**)calloc(nb_channel, sizeof(channel_t*));
  if ((*callback)->channels == NULL) {
    return SNAIL_ERR;
  }

  return SNAIL_OK;
}


void destroy_callback(callback_t* callback) {
  assert(callback != NULL);

  int i;
  for (i = 0; i <= callback->nb_channel - 1; i++) {
    destroy_channel(callback->channels[i]);
  }
  free(callback->channels);
  free(callback);
  callback = NULL;
}


int create_channel(channel_t** channel, const char* name) {
  assert(*channel == NULL);
  
  *channel = (channel_t*)malloc(sizeof(channel_t));
  if (*channel == NULL) {
    return SNAIL_ERR;
  }
  (*channel)->name = strdup(name);
  if ((*channel)->name == NULL) {
    return SNAIL_ERR;
  }
  (*channel)->ikey = 0;
  (*channel)->flags = 0;

  return SNAIL_OK;
}


int create_timer_channel(channel_t** channel, uint64_t ikey) {
  assert(*channel == NULL);
  
  *channel = (channel_t*)malloc(sizeof(channel_t));
  if (*channel == NULL) {
    return SNAIL_ERR;
  }
  (*channel)->name = NULL;
  (*channel)->ikey = ikey;

  (*channel)->flags = CHANNEL_TIMER_EVENT;

  return SNAIL_OK;
}


void destroy_channel(channel_t* channel) {
  assert(channel != NULL);

  if (channel->name != NULL) {
    free(channel->name);
  }
  free(channel);
  channel = NULL;
}


int insert(node_t **root, node_t **leaf, const char* key) {

  int len = strlen(key);

  if (*root == NULL) { 
    *root = (node_t*)malloc(sizeof(node_t));
    if (*root == NULL) {
      return SNAIL_ERR;
    }
    (*root)->key = strdup(key);
    (*root)->left = NULL;    
    (*root)->right = NULL;
    (*root)->cb_list = (callback_ends_t*)malloc(sizeof(callback_ends_t));
    if ((*root)->cb_list == NULL) {
      return SNAIL_ERR;
    }
    (*root)->cb_list->head = NULL;
    (*root)->cb_list->tail = NULL;
    (*root)->data = NULL;
    *leaf = *root;
  } else if (strncmp(key, (*root)->key, len) < 0) {
    return insert(&(*root)->left, &(*leaf), key);
  } else if (strncmp(key, (*root)->key, len) > 0) {
    return insert(&(*root)->right, &(*leaf), key);
  } else {
    *leaf = *root;
  }

  return SNAIL_OK;
}


void destroy_tree(node_t **root) {
  if (root == NULL || (*root) == NULL) {
    return;
  }
  if ((*root)->left != NULL) {
    destroy_tree(&(*root)->left);
  }
  if ((*root)->right != NULL) {
    destroy_tree(&(*root)->right);
  }
  if ((*root)->key != NULL) {
    free((*root)->key);
  }
  if ((*root)->data != NULL) {
    stop_timer((*root)->data);
  }

  destroy_list(&(*root)->cb_list);
  free((*root)->cb_list);
  free((*root));
  *root = NULL;
}


void destroy_list(callback_ends_t **cb_list) {
  if (cb_list == NULL || (*cb_list) == NULL) {
    return;
  }
  callback_ll_t *temp = (*cb_list)->head;
  while (temp != NULL) {
    callback_ll_t *next = temp->next;
    destroy_wrapper(&temp);
    temp = next;
  }
  (*cb_list)->head = NULL;
  (*cb_list)->tail = NULL;
  /* don't do *cb_list = NULL */
}


void search(const char* key, node_t *leaf, callback_ends_t** cb_list) {
  assert(leaf != NULL);

  int len = strlen(key);
  if (strncmp(key, leaf->key, len) == 0) {
    *cb_list = leaf->cb_list;
  } else if (strncmp(key, leaf->key, len) < 0) {
    search(key, leaf->left, cb_list);
  } else {
    search(key, leaf->right, cb_list);
  }
}


int insert_timer(node_t **root, node_t **leaf, uint64_t key) {

  if (*root == NULL) { 
    *root = (node_t*)malloc(sizeof(node_t));
    if (*root == NULL) {
      return SNAIL_ERR;
    }
    (*root)->key = NULL;
    (*root)->ikey = key;
    (*root)->left = NULL;    
    (*root)->right = NULL;
    (*root)->cb_list = (callback_ends_t*)malloc(sizeof(callback_ends_t));
    if ((*root)->cb_list == NULL) {
      return SNAIL_ERR;
    }
    (*root)->cb_list->head = NULL;
    (*root)->cb_list->tail = NULL;
    (*root)->data = NULL;
    *leaf = *root;
    
    return 2;
  } else if (key < (*root)->ikey) {
    return insert_timer(&(*root)->left, &(*leaf), key);
  } else if (key > (*root)->ikey) {
    return insert_timer(&(*root)->right, &(*leaf), key);
  } else {
    *leaf = *root;
  }

  return SNAIL_OK;
}

void search_timer(uint64_t key, node_t *root, node_t** leaf) {
  assert(root != NULL);

  if (key == root->ikey) {
    *leaf = root;
  } else if (key < root->ikey) {
    search_timer(key, root->left, leaf);
  } else {
    search_timer(key, root->right, leaf);
  }
}


void dump_tree(node_t* node) {
  
  if (node == NULL) {
    return;
  }
  
  if (node->left != NULL) {
    dump_tree(node->left);
  }
  
  if (node->right != NULL) {
    dump_tree(node->right);
  }
  
  if (node->key != NULL) {
    printf("%p, key: %s, left: %p, right: %p, data: %p\n", node, node->key, node->left, node->right, node->data);
  } else {
    printf("%p, key: %" PRIu64 ", left: %p, right: %p, data: %p\n", node, node->ikey, node->left, node->right, node->data);
  }

  if (node->cb_list == NULL) {
    return;
  }

  dump_list(node->cb_list);
}

void dump_list(callback_ends_t* cb_list_ends) {
  if (cb_list_ends == NULL) {
    return;
  }
  int nb = 0;
  callback_ll_t* cb_list = cb_list_ends->head;
  while (cb_list != NULL) {
    callback_t *cb = cb_list->cb;
    printf("ref: %i, flags: %i, nb: %i\n", cb->ref, cb->flags, cb->nb_channel);
    int i;
    for (i = 0; i <= cb->nb_channel - 1; i++) {
      channel_t *ch = cb->channels[i];
      if (ch->name != NULL) {
        printf("name: %s, flags: %i\n", ch->name, ch->flags);
      } else {
        printf("name: %" PRIu64 ", flags: %i\n", ch->ikey, ch->flags);
      }
    }
    cb_list = cb_list->next;
    nb++;
  }
  printf("cb_list: %i\n", nb);
}
