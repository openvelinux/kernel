#ifndef RPAL_H_INCLUDED
#define RPAL_H_INCLUDED

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

#include <stdint.h>
#include <stdarg.h>
#include <sys/epoll.h>

typedef enum rpal_error_code {
	RPAL_ERR_NONE = 0,
	RPAL_ERR_BAD_ARG = 1,
	RPAL_ERR_NO_SERVICE = 2,
	RPAL_ERR_MAPPED = 3,
	RPAL_ERR_RETRY = 4,
	RPAL_ERR_BAD_SERVICE_STATUS = 5,
	RPAL_ERR_BAD_THREAD_STATUS = 6,
	RPAL_ERR_REACH_LIMIT = 7,
	RPAL_ERR_NOMEM = 8,
	RPAL_ERR_NOMAPPING = 9,
	RPAL_ERR_INVAL = 10,

	RPAL_ERR_KERNEL_MAX_CODE = 100,

	RPAL_ERR_RPALFILE_OPS, /**< Failed to open /proc/self/rpal */
	RPAL_ERR_RPAL_DISABLED,
	RPAL_ERR_GET_CAP_VERSION,
	RPAL_KERNEL_API_NOTSUPPORT,
	RPAL_HARDWARE_NOTSUPPORT,
	RPAL_ERR_SERVICE_KEY, /**< Failed to get service key */
	RPAL_ERR_SENDERS_METADATA,
	RPAL_ERR_ENABLE_SERVICE,
	RPAL_ERR_SENDER_PAGES,
	RPAL_DONT_INITED,
	RPAL_ERR_SENDER_INIT,
	RPAL_ERR_SENDER_REG,
	RPAL_INVALID_ARG,
	RPAL_CACHE_FULL,
	RPAL_FDE_OUTDATED,
	RPAL_QUEUE_PUT_FAILED,
	RPAL_ERR_PEER_MEM,
	RPAL_ERR_NOTIFY_RECVER,
	RPAL_INVAL_THREAD,
	RPAL_INVAL_SERVICE,
} rpal_error_code_t;

#define EPOLLRPALIN 0x00020000
#define EPOLLRPALOUT 0x00040000

typedef enum rpal_features {
	RPAL_SENDER_RECEIVER = 0x1 << 0,
} rpal_features_t;

typedef enum status {
	RPAL_FAILURE = -1, /**< return value indicating failure */
	RPAL_SUCCESS /**< return value indicating success */
} status_t;

#define RPAL_PUBLIC __attribute__((visibility("default")))

RPAL_PUBLIC
int rpal_init(int nr_rpalthread, int flags, rpal_error_code_t *error);

RPAL_PUBLIC
void rpal_exit(void);

RPAL_PUBLIC
int rpal_is_enabled(void);

RPAL_PUBLIC
int rpal_is_enabled_fast(void);

RPAL_PUBLIC
int rpal_thread_init(void);

RPAL_PUBLIC
void rpal_thread_exit(void);

RPAL_PUBLIC
int rpal_request_service(uint64_t key);

RPAL_PUBLIC
status_t rpal_release_service(uint64_t key);

RPAL_PUBLIC
status_t rpal_clean_service_start(int64_t *ptr);

RPAL_PUBLIC
void rpal_clean_service_end(int64_t *ptr);

RPAL_PUBLIC
int rpal_get_service_id(void);

RPAL_PUBLIC
status_t rpal_get_service_key(uint64_t *service_key);

RPAL_PUBLIC
int rpal_get_request_service_id(uint64_t key);

RPAL_PUBLIC
status_t rpal_uds_fdmap(uint64_t sid_fd, uint64_t *rpalfd);

RPAL_PUBLIC
int rpal_get_peer_rid(uint64_t sid_fd);

RPAL_PUBLIC
status_t rpal_sender_init(rpal_error_code_t *error);

RPAL_PUBLIC
status_t rpal_sender_exit(void);

/* Hook epoll syscall */
RPAL_PUBLIC
int rpal_epoll_wait(int epfd, struct epoll_event *events, int maxevents,
		    int timeout);

RPAL_PUBLIC
int rpal_epoll_wait_user(int epfd, struct epoll_event *events, int maxevents,
			 int timeout);

RPAL_PUBLIC
int rpal_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);

RPAL_PUBLIC
status_t rpal_copy_prepare(int service_id);

RPAL_PUBLIC
status_t rpal_copy_finish(void);

RPAL_PUBLIC
int rpal_write_ptrs(int service_id, uint64_t rpalfd, int64_t *ptrs, int len);

RPAL_PUBLIC
int rpal_read_ptrs(int fd, int64_t *ptrs, int len);

typedef int (*access_fn)(va_list args);
RPAL_PUBLIC
status_t rpal_read_access_safety(access_fn do_access, int *do_access_ret, ...);

RPAL_PUBLIC
void rpal_recver_count_print(void);

RPAL_PUBLIC
void rpal_sender_count_print(void);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif
#endif //!_RPAL_H_INCLUDED
