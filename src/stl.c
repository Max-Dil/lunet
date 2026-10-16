#include "stl.h"

#include <stdlib.h>


queue_t* queue_init(void) {
  queue_t* queue = malloc(sizeof(queue_t));
  if (!queue) return NULL;
  queue->head = NULL;
  queue->tail = NULL;
  queue->size = 0;
  return queue;
}


void queue_destroy(queue_t* queue) {
  if (!queue) return;

  while (!queue_is_empty(queue)) {
    queue_dequeue(queue);
  }
  free(queue);
}


int queue_enqueue(queue_t* queue, void* data) {
  if (!queue) return -1;

  queue_node_t* node = malloc(sizeof(queue_node_t));
  if (!node) return -1;  

  node->data = data;
  node->next = NULL;

  if (queue->tail) {
    queue->tail->next = node;
  } else {
    
    queue->head = node;
  }

  queue->tail = node;
  queue->size++;
  return 0;
}


void* queue_dequeue(queue_t* queue) {
  if (!queue || queue_is_empty(queue)) {
    return NULL;
  }

  queue_node_t* node = queue->head;
  void* data = node->data;

  queue->head = node->next;
  if (!queue->head) {
    
    queue->tail = NULL;
  }

  free(node);
  queue->size--;
  return data;
}


void* queue_peek(queue_t* queue) {
  if (!queue || queue_is_empty(queue)) {
    return NULL;
  }

  return queue->head->data;
}


bool queue_is_empty(queue_t* queue) { return !queue || queue->head == NULL; }


size_t queue_size(queue_t* queue) { return queue ? queue->size : 0; }