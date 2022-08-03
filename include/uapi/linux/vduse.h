/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_VDUSE_H_
#define _UAPI_VDUSE_H_

#include <linux/types.h>

#define VDUSE_API_VERSION	0xff

#define VDUSE_MAX_TRANSFER_LEN	256
#define VDUSE_NAME_MAX	256

/* the control messages definition for read/write */

enum vduse_req_type {
	/* Set the vring address of virtqueue. */
	VDUSE_SET_VQ_NUM,
	/* Set the vring address of virtqueue. */
	VDUSE_SET_VQ_ADDR,
	/* Set ready status of virtqueue */
	VDUSE_SET_VQ_READY,
	/* Get ready status of virtqueue */
	VDUSE_GET_VQ_READY,
	/* Set the state for virtqueue */
	VDUSE_SET_VQ_STATE,
	/* Get the state for virtqueue */
	VDUSE_GET_VQ_STATE,
	/* Set virtio features supported by the driver */
	VDUSE_SET_FEATURES,
	/* Get virtio features supported by the device */
	VDUSE_GET_FEATURES,
	/* Set the device status */
	VDUSE_SET_STATUS,
	/* Get the device status */
	VDUSE_GET_STATUS,
	/* Write to device specific configuration space */
	VDUSE_SET_CONFIG,
	/* Read from device specific configuration space */
	VDUSE_GET_CONFIG,
	/* Notify userspace to update the memory mapping in device IOTLB */
	VDUSE_UPDATE_IOTLB,
	VDUSE_VDPA_DISCONNECT = 256,
};

struct vduse_vq_num {
	__u32 index; /* virtqueue index */
	__u32 num; /* the size of virtqueue */
};

struct vduse_vq_addr {
	__u32 index; /* virtqueue index */
	__u32 padding; /* padding */
	__u64 desc_addr; /* address of desc area */
	__u64 driver_addr; /* address of driver area */
	__u64 device_addr; /* address of device area */
};

struct vduse_vq_ready {
	__u32 index; /* virtqueue index */
	__u8 ready; /* ready status of virtqueue */
};

struct vduse_vq_state {
	__u32 index; /* virtqueue index */
	__u32 avail_idx; /* virtqueue state (last_avail_idx) */
};

struct vduse_dev_config_data {
	__u32 offset; /* offset from the beginning of config space */
	__u32 len; /* the length to read/write */
	__u8 data[VDUSE_MAX_TRANSFER_LEN]; /* data buffer used to read/write */
};

struct vduse_iova_range {
	__u64 start; /* start of the IOVA range */
	__u64 last; /* end of the IOVA range */
};

struct vduse_features {
	__u64 features; /* virtio features */
};

struct vduse_status {
	__u8 status; /* device status */
};

struct vduse_dev_request {
	__u32 type; /* request type */
	__u32 request_id; /* request id */
	__u32 reserved[2]; /* for future use */
	union {
		struct vduse_vq_num vq_num; /* virtqueue num */
		struct vduse_vq_addr vq_addr; /* virtqueue address */
		struct vduse_vq_ready vq_ready; /* virtqueue ready status */
		struct vduse_vq_state vq_state; /* virtqueue state */
		struct vduse_dev_config_data config; /* virtio device config space */
		struct vduse_iova_range iova; /* iova range for updating */
		struct vduse_features f; /* virtio features */
		struct vduse_status s; /* device status */
		__u32 padding[128]; /* padding */
	};
};

struct vduse_dev_response {
	__u32 request_id; /* corresponding request id */
#define VDUSE_REQUEST_OK	0x00
#define VDUSE_REQUEST_FAILED	0x01
	__u32 result; /* the result of request */
	__u32 reserved[2]; /* for future use */
	union {
		struct vduse_vq_ready vq_ready; /* virtqueue ready status */
		struct vduse_vq_state vq_state; /* virtqueue state */
		struct vduse_dev_config_data config; /* virtio device config space */
		struct vduse_features f; /* virtio features */
		struct vduse_status s; /* device status */
		__u32 padding[128]; /* padding */
	};
};

/* ioctls */

struct vduse_dev_config {
	char name[VDUSE_NAME_MAX]; /* vduse device name */
	__u32 vendor_id; /* virtio vendor id */
	__u32 device_id; /* virtio device id */
	__u64 bounce_size; /* bounce buffer size for iommu */
	__u16 vq_size_max; /* the max size of virtqueue */
	__u16 padding; /* padding */
	__u32 vq_num; /* the number of virtqueues */
	__u32 vq_align; /* the allocation alignment of virtqueue's metadata */
	__u32 config_size; /* the size of the configuration space */
	__u32 reserved[5]; /* for future use */
	__u16 req_cached; /* cached request mask */
	__u16 dev_shm_size; /* size of device shared memory */
	__u16 vq_shm_off; /* offset of virtqueue shared memory */
	__u16 dead_timeout; /* dead timeout */
};

struct vduse_iotlb_entry {
	__u64 offset; /* the mmap offset on fd */
	__u64 start; /* start of the IOVA range */
	__u64 last; /* last of the IOVA range */
#define VDUSE_ACCESS_RO 0x1
#define VDUSE_ACCESS_WO 0x2
#define VDUSE_ACCESS_RW 0x3
	__u8 perm; /* access permission of this range */
};

struct vduse_vq_eventfd {
	__u32 index; /* virtqueue index */
#define VDUSE_EVENTFD_DEASSIGN -1
	int fd; /* eventfd, -1 means de-assigning the eventfd */
};

#define VDUSE_SHM_ALIGNMENT 64

struct desc_state_split {
	__u8 inflight;
	__u8 padding[5];
	__u8 next;
	__u8 counter;
};

struct vduse_vq_inflight {
	__u64 features;
	__u16 version;
	__u16 desc_num;
	__u16 last_batch_head;
	__u16 used_idx;
	struct desc_state_split desc[];
};

struct vduse_vq_state_split {
	__u16 avail_index;
};

struct vduse_vq_state_packed {
	__u16 last_avail_counter;
	__u16 last_avail_idx;
	__u16 last_used_counter;
	__u16 last_used_idx;
};

struct vduse_vq_info {
	__u32 index;
	__u32 num;
	__u64 desc_addr;
	__u64 driver_addr;
	__u64 device_addr;
	union {
		struct vduse_vq_state_split split;
		struct vduse_vq_state_packed packed;
	};
	__u8 ready;
};

#define VDUSE_BASE	0x81

/* Get the version of VDUSE API. This is used for future extension */
#define VDUSE_GET_API_VERSION	_IO(VDUSE_BASE, 0x00)

/* Set the version of VDUSE API. */
#define VDUSE_SET_API_VERSION	_IO(VDUSE_BASE, 0x01)

/* Create a vduse device which is represented by a char device (/dev/vduse/<name>) */
#define VDUSE_CREATE_DEV	_IOW(VDUSE_BASE, 0x02, struct vduse_dev_config)

/* Destroy a vduse device. Make sure there are no references to the char device */
#define VDUSE_DESTROY_DEV	_IOW(VDUSE_BASE, 0x03, char[VDUSE_NAME_MAX])

/*
 * Get a file descriptor for the first overlapped iova region,
 * -EINVAL means the iova region doesn't exist.
 */
#define VDUSE_IOTLB_GET_FD	_IOWR(VDUSE_BASE, 0x04, struct vduse_iotlb_entry)

/* Setup an eventfd to receive kick for virtqueue */
#define VDUSE_VQ_SETUP_KICKFD	_IOW(VDUSE_BASE, 0x05, struct vduse_vq_eventfd)

/* Inject an interrupt for specific virtqueue */
#define VDUSE_INJECT_VQ_IRQ	_IOW(VDUSE_BASE, 0x06, __u32)

/* Inject a config interrupt */
#define VDUSE_INJECT_CONFIG_IRQ	_IO(VDUSE_BASE, 0x07)

/*
 * Get the negotiated virtio features. It's a subset of the features in
 * struct vduse_dev_config which can be accepted by virtio driver. It's
 * only valid after FEATURES_OK status bit is set.
 */
#define VDUSE_DEV_GET_FEATURES	_IOR(VDUSE_BASE, 0x11, __u64)

/* Get the specified virtqueue's information. Caller should set index field. */
#define VDUSE_VQ_GET_INFO	_IOWR(VDUSE_BASE, 0x15, struct vduse_vq_info)

/**
 * struct vduse_iova_umem - userspace memory configuration for one IOVA region
 * @uaddr: start address of userspace memory, it must be aligned to page size
 * @iova: start of the IOVA region
 * @size: size of the IOVA region
 * @reserved: for future use, needs to be initialized to zero
 *
 * Structure used by VDUSE_IOTLB_REG_UMEM and VDUSE_IOTLB_DEREG_UMEM
 * ioctls to register/de-register userspace memory for IOVA regions
 */
struct vduse_iova_umem {
	__u64 uaddr;
	__u64 iova;
	__u64 size;
	__u64 reserved[3];
};

/* Register userspace memory for IOVA regions */
#define VDUSE_IOTLB_REG_UMEM	_IOW(VDUSE_BASE, 0x18, struct vduse_iova_umem)

/* De-register the userspace memory. Caller should set iova and size field. */
#define VDUSE_IOTLB_DEREG_UMEM	_IOW(VDUSE_BASE, 0x19, struct vduse_iova_umem)

#endif /* _UAPI_VDUSE_H_ */
