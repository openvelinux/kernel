#ifndef RPAL_DEBUG_H
#define RPAL_DEBUG_H

typedef enum {
	RPAL_DEBUG_MANAGEMENT = (1 << 0),
	RPAL_DEBUG_SENDER = (1 << 1),
	RPAL_DEBUG_RECVER = (1 << 2),
	RPAL_DEBUG_FIBER = (1 << 3),

	__RPAL_DEBUG_ALL = ~(0ULL),
} rpal_debug_flag_t;
#endif
