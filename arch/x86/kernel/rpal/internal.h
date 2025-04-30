/* SPDX-License-Identifier: GPL-2.0 */

#define RPAL_COMPAT_VERSION 1
#define RPAL_API_VERSION 1

/* Error Code */
#define RPAL_ERR_BAD_ARG 1
#define RPAL_ERR_NO_SERVICE 2
#define RPAL_ERR_MAPPED 3
#define RPAL_ERR_RETRY 4
#define RPAL_ERR_BAD_SERVICE_STATUS 5
#define RPAL_ERR_BAD_THREAD_STATUS 6
#define RPAL_ERR_REACH_LIMIT 7
#define RPAL_ERR_NOMEM 8
#define RPAL_ERR_NOMAPPING 9
#define RPAL_ERR_INVAL 10

/* rpal_ctl command */
enum rpal_command_type {
	RPAL_CMD_GET_API_VERSION_AND_CAP,
	RPAL_CMD_GET_SERVICE_KEY,
	RPAL_CMD_REQUEST_SERVICE,
	RPAL_CMD_RELEASE_SERVICE,
	RPAL_CMD_ENABLE_SERVICE,
	RPAL_CMD_DISABLE_SERVICE,
	RPAL_NR_CMD,
};

enum {
	RPAL_REQUEST_MAP,
	RPAL_REVERSE_MAP,
};

extern bool rpal_inited;

/* service.c */
int __init rpal_service_init(void);
void rpal_service_exit(void);
long rpal_ctl(unsigned long cmd, unsigned long arg0, unsigned long arg1);
long rpal_request_service(u64 key, void __user *to);
long rpal_release_service(u64 key);
long rpal_enable_service(void __user *u_data, void __user *k_data, bool is_new);
long rpal_disable_service(void);

/* mm.c */
int rpal_mmap(struct file *filp, struct vm_area_struct *vma);
int rpal_map_service(struct rpal_service *tgt);
void rpal_unmap_service(struct rpal_service *tgt);
