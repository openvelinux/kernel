#include "rpal_queue.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define min(X, Y) ({ ((X) > (Y)) ? (Y) : (X); })

static unsigned int roundup_pow_of_two(unsigned int data)
{
	unsigned int msb_position;

	if (data <= 1)
		return 1;
	if (!(data & (data - 1)))
		return data;

	msb_position = 31 - __builtin_clz(data);
	assert(msb_position < 31);
	return 1 << (msb_position + 1);
}

QUEUE_UINT rpal_queue_unused(rpal_queue_t *q)
{
	return (q->mask + 1) - (q->tail - q->head);
}

QUEUE_UINT rpal_queue_len(rpal_queue_t *q)
{
	return (q->tail - q->head);
}

int rpal_queue_init(rpal_queue_t *q, void *data, QUEUE_UINT_INC usize)
{
	QUEUE_UINT_INC size;
	if (usize > QUEUE_UINT_MAX || !data) {
		return -1;
	}
	size = roundup_pow_of_two(usize);
	if (usize != size) {
		return -1;
	}
	q->data = data;
	memset(q->data, 0, size * sizeof(int64_t));
	q->head = 0;
	q->tail = 0;
	q->mask = size - 1;
	return 0;
}

void *rpal_queue_destroy(rpal_queue_t *q)
{
	void *data = q->data;
	if (q->data) {
		q->data = NULL;
	}
	q->mask = 0;
	q->head = 0;
	q->tail = 0;
	return data;
}

int rpal_queue_alloc(rpal_queue_t *q, QUEUE_UINT_INC size)
{
	assert(q && size);
	if (size > QUEUE_UINT_MAX) {
		return -1;
	}
	size = roundup_pow_of_two(size);
	q->data = malloc(size * sizeof(int64_t));
	if (!q->data)
		return -1;
	memset(q->data, 0, size * sizeof(int64_t));
	q->head = 0;
	q->tail = 0;
	q->mask = size - 1;
	return 0;
}

void rpal_queue_free(rpal_queue_t *q)
{
	if (q->data) {
		free(q->data);
		q->data = NULL;
	}
	q->mask = 0;
	q->head = 0;
	q->tail = 0;
}

static void rpal_queue_copy_in(rpal_queue_t *q, const int64_t *buf,
			       QUEUE_UINT_INC len, QUEUE_UINT off)
{
	QUEUE_UINT_INC l;
	QUEUE_UINT_INC size = q->mask + 1;

	off &= q->mask;
	l = min(len, size - off);

	memcpy(q->data + off, buf, l << 3);
	memcpy(q->data, buf + l, (len - l) << 3);
	asm volatile("" : : : "memory");
}

QUEUE_UINT_INC rpal_queue_put(rpal_queue_t *q, const int64_t *buf,
			      QUEUE_UINT_INC len)
{
	QUEUE_UINT_INC l;

	if (!q->data) {
		return 0;
	}
	l = rpal_queue_unused(q);
	if (len > l) {
		return 0;
	}
	l = len;
	rpal_queue_copy_in(q, buf, l, q->tail);
	q->tail += l;
	return l;
}

static QUEUE_UINT_INC rpal_queue_copy_out(rpal_queue_t *q, int64_t *buf,
					  QUEUE_UINT_INC len, QUEUE_UINT head)
{
	unsigned int l;
	QUEUE_UINT tail;
	QUEUE_UINT off;
	QUEUE_UINT_INC size = q->mask + 1;

	tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
	len = min((QUEUE_UINT)(tail - head), len);
	if (head == tail)
		return 0;
	off = head & q->mask;
	l = min(len, size - off);

	memcpy(buf, q->data + off, l << 3);
	memcpy(buf + l, q->data, (len - l) << 3);

	return len;
}

QUEUE_UINT_INC rpal_queue_peek(rpal_queue_t *q, int64_t *buf,
			       QUEUE_UINT_INC len, QUEUE_UINT *phead)
{
	QUEUE_UINT_INC copied;
	QUEUE_UINT head;

	head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
	copied = rpal_queue_copy_out(q, buf, len, head);
	if (phead) {
		*phead = head;
	}
	return copied;
}

QUEUE_UINT_INC rpal_queue_skip(rpal_queue_t *q, QUEUE_UINT head,
			       QUEUE_UINT_INC skip)
{
	if (skip > rpal_queue_len(q)) {
		return 0;
	}
	if (__atomic_compare_exchange_n(&q->head, &head, head + skip, 1,
					__ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
		return skip;
	}
	return 0;
}

QUEUE_UINT_INC rpal_queue_get(rpal_queue_t *q, int64_t *buf, QUEUE_UINT_INC len)
{
	QUEUE_UINT_INC copied;
	QUEUE_UINT head;

	while (1) {
		head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
		copied = rpal_queue_copy_out(q, buf, len, head);
		if (__atomic_compare_exchange_n(&q->head, &head, head + copied,
						1, __ATOMIC_RELAXED,
						__ATOMIC_RELAXED)) {
			return copied;
		}
	}
}

void rpal_uevent_queue_init(epoll_uevent_queue_t *ueventq,
			    volatile uint64_t *uqlock)
{
	int i;
	__atomic_store_n(uqlock, (uint64_t)0, __ATOMIC_RELAXED);
	ueventq->l_beg = 0;
	ueventq->l_end = 0;
	ueventq->l_end_cache = 0;
	for (i = 0; i < MAX_RDY; ++i) {
		ueventq->fds[i] = -1;
	}
	return;
}

QUEUE_UINT uevent_queue_len(epoll_uevent_queue_t *ueventq)
{
	return (ueventq->l_end - ueventq->l_beg);
}

QUEUE_UINT uevent_queue_add(epoll_uevent_queue_t *ueventq, int fd)
{
	unsigned int pos;
	if (uevent_queue_len(ueventq) == MAX_RDY)
		return MAX_RDY;
	pos = __sync_fetch_and_add(&ueventq->l_end_cache, 1);
	pos %= MAX_RDY;
	ueventq->fds[pos] = fd;
	asm volatile("" : : : "memory");
	__sync_fetch_and_add(&ueventq->l_end, 1);
	return (pos);
}

int uevent_queue_del(epoll_uevent_queue_t *ueventq)
{
	int fd = -1;
	int pos;
	if (uevent_queue_len(ueventq) == 0) {
		return -1;
	}
	pos = ueventq->l_beg % MAX_RDY;
	fd = ueventq->fds[pos];
	asm volatile("" : : : "memory");
	__sync_fetch_and_add(&ueventq->l_beg, 1);
	return fd;
}

int uevent_queue_fix(epoll_uevent_queue_t *ueventq)
{
	__atomic_store_n(&ueventq->l_end_cache, ueventq->l_end,
			 __ATOMIC_SEQ_CST);
	return 0;
}
