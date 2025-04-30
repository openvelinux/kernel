#ifndef RPAL_QUEUE_H
#define RPAL_QUEUE_H

#include <stdint.h>

// typedef uint8_t QUEUE_UINT;
// typedef uint16_t QUEUE_UINT_INC;
// #define QUEUE_UINT_MAX UINT8_MAX

// typedef uint16_t QUEUE_UINT;
// typedef uint32_t QUEUE_UINT_INC;
// #define QUEUE_UINT_MAX UINT16_MAX

typedef uint32_t QUEUE_UINT;
typedef uint64_t QUEUE_UINT_INC;
#define QUEUE_UINT_MAX UINT32_MAX

typedef struct rpal_queue {
	QUEUE_UINT head;
	QUEUE_UINT tail;
	QUEUE_UINT mask;
	uint64_t *data;
} rpal_queue_t;

QUEUE_UINT rpal_queue_len(rpal_queue_t *q);
QUEUE_UINT rpal_queue_unused(rpal_queue_t *q);
int rpal_queue_init(rpal_queue_t *q, void *data, QUEUE_UINT_INC usize);
void *rpal_queue_destroy(rpal_queue_t *q);
int rpal_queue_alloc(rpal_queue_t *q, QUEUE_UINT_INC size);
void rpal_queue_free(rpal_queue_t *q);
QUEUE_UINT_INC rpal_queue_put(rpal_queue_t *q, const int64_t *buf,
			      QUEUE_UINT_INC len);
QUEUE_UINT_INC rpal_queue_get(rpal_queue_t *q, int64_t *buf,
			      QUEUE_UINT_INC len);
QUEUE_UINT_INC rpal_queue_peek(rpal_queue_t *q, int64_t *buf,
			       QUEUE_UINT_INC len, QUEUE_UINT *phead);
QUEUE_UINT_INC rpal_queue_skip(rpal_queue_t *q, QUEUE_UINT head,
			       QUEUE_UINT_INC skip);

#define MAX_RDY 4096
typedef struct epoll_uevent_queue {
	int fds[MAX_RDY];
	volatile QUEUE_UINT l_beg;
	volatile QUEUE_UINT l_end;
	volatile QUEUE_UINT l_end_cache;
} epoll_uevent_queue_t;

void rpal_uevent_queue_init(epoll_uevent_queue_t *ueventq,
			    volatile uint64_t *uqlock);
QUEUE_UINT uevent_queue_len(epoll_uevent_queue_t *ueventq);
QUEUE_UINT uevent_queue_add(epoll_uevent_queue_t *ueventq, int fd);
int uevent_queue_del(epoll_uevent_queue_t *ueventq);
int uevent_queue_fix(epoll_uevent_queue_t *ueventq);

#endif
