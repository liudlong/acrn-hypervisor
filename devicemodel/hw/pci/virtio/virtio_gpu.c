/*
 * Copyright (C) 2022 Intel Corporation.
 *
 * virtio-gpu device
 *
 */
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>

#include "dm.h"
#include "pci_core.h"
#include "virtio.h"
#include "vdisplay.h"

/*
 * Queue definitions.
 */
#define VIRTIO_GPU_CONTROLQ	0
#define VIRTIO_GPU_CURSORQ	1
#define VIRTIO_GPU_QNUM		2

/*
 * Virtqueue size.
 */
#define VIRTIO_GPU_RINGSZ	64
#define VIRTIO_GPU_MAXSEGS	256

/*
 * Feature bits
 */
#define VIRTIO_GPU_F_EDID		1
#define VIRTIO_GPU_F_RESOURCE_UUID	2
#define VIRTIO_GPU_F_RESOURCE_BLOB	3
#define VIRTIO_GPU_F_CONTEXT_INIT	4

/*
 * Host capabilities
 */
#define VIRTIO_GPU_S_HOSTCAPS	(1UL << VIRTIO_F_VERSION_1) | \
				(1UL << VIRTIO_GPU_F_EDID)

/*
 * Device events
 */
#define VIRTIO_GPU_EVENT_DISPLAY	(1 << 0)

/*
 * Generic definitions
 */
#define VIRTIO_GPU_MAX_SCANOUTS 16
#define VIRTIO_GPU_FLAG_FENCE (1 << 0)
#define VIRTIO_GPU_FLAG_INFO_RING_IDX (1 << 1)

/*
 * Config space "registers"
 */
struct virtio_gpu_config {
	uint32_t events_read;
	uint32_t events_clear;
	uint32_t num_scanouts;
	uint32_t num_capsets;
};

/*
 * Common
 */
enum virtio_gpu_ctrl_type {
	/* 2d commands */
	VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
	VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
	VIRTIO_GPU_CMD_RESOURCE_UNREF,
	VIRTIO_GPU_CMD_SET_SCANOUT,
	VIRTIO_GPU_CMD_RESOURCE_FLUSH,
	VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
	VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
	VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,
	VIRTIO_GPU_CMD_GET_CAPSET_INFO,
	VIRTIO_GPU_CMD_GET_CAPSET,
	VIRTIO_GPU_CMD_GET_EDID,
	VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID,
	VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB,
	VIRTIO_GPU_CMD_SET_SCANOUT_BLOB,

	/* cursor commands */
	VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
	VIRTIO_GPU_CMD_MOVE_CURSOR,

	/* success responses */
	VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
	VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
	VIRTIO_GPU_RESP_OK_CAPSET_INFO,
	VIRTIO_GPU_RESP_OK_CAPSET,
	VIRTIO_GPU_RESP_OK_EDID,
	VIRTIO_GPU_RESP_OK_RESOURCE_UUID,
	VIRTIO_GPU_RESP_OK_MAP_INFO,

	/* error responses */
	VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
	VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
	VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
	VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
	VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
	VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

struct virtio_gpu_ctrl_hdr {
	uint32_t type;
	uint32_t flags;
	uint64_t fence_id;
	uint32_t ctx_id;
	uint8_t ring_idx;
	uint8_t padding[3];
};

/*
 * Command: VIRTIO_GPU_CMD_GET_EDID
 */
struct virtio_gpu_get_edid {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t scanout;
	uint32_t padding;
};

struct virtio_gpu_resp_edid {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t size;
	uint32_t padding;
	uint8_t edid[1024];
};

/*
 * Command: VIRTIO_GPU_CMD_GET_DISPLAY_INFO
 */
struct virtio_gpu_rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct virtio_gpu_resp_display_info {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_display_one {
		struct virtio_gpu_rect r;
		uint32_t enabled;
		uint32_t flags;
	} pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

/*
 * Command: VIRTIO_GPU_CMD_RESOURCE_CREATE_2D
 */
enum virtio_gpu_formats {
	VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1,
	VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
	VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3,
	VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4,

	VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 67,
	VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM = 68,

	VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM = 121,
	VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM = 134,
};

struct virtio_gpu_resource_create_2d {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t resource_id;
	uint32_t format;
	uint32_t width;
	uint32_t height;
};

struct virtio_gpu_resource_2d {
	uint32_t resource_id;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	pixman_image_t *image;
	struct iovec *iov;
	uint32_t iovcnt;
	LIST_ENTRY(virtio_gpu_resource_2d) link;
};

/*
 * Command: VIRTIO_GPU_CMD_RESOURCE_UNREF
 */
struct virtio_gpu_resource_unref {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t resource_id;
	uint32_t padding;
};

/*
 * Command: VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING
 */
struct virtio_gpu_mem_entry {
	uint64_t addr;
	uint32_t length;
	uint32_t padding;
};

struct virtio_gpu_resource_attach_backing {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t resource_id;
	uint32_t nr_entries;
};

/*
 * Command: VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING
 */
struct virtio_gpu_resource_detach_backing {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t resource_id;
	uint32_t padding;
};

/*
 * Command: VIRTIO_GPU_CMD_SET_SCANOUT
 */
struct virtio_gpu_set_scanout {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_rect r;
	uint32_t scanout_id;
	uint32_t resource_id;
};

/*
 * Command: VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D
 */
struct virtio_gpu_transfer_to_host_2d {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_rect r;
	uint64_t offset;
	uint32_t resource_id;
	uint32_t padding;
};

/*
 * Command: VIRTIO_GPU_CMD_RESOURCE_FLUSH
 */
struct virtio_gpu_resource_flush {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_rect r;
	uint32_t resource_id;
	uint32_t padding;
};

/*
 * Per-device struct
 */
struct virtio_gpu {
	struct virtio_base base;
	struct virtio_vq_info vq[VIRTIO_GPU_QNUM];
	struct virtio_gpu_config cfg;
	pthread_mutex_t	mtx;
	int vdpy_handle;
	LIST_HEAD(,virtio_gpu_resource_2d) r2d_list;
	struct vdpy_display_bh ctrl_bh;
};

struct virtio_gpu_command {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu *gpu;
	struct virtio_vq_info *vq;
	struct iovec *iov;
	uint32_t iovcnt;
	bool finished;
	uint32_t iolen;
};

static void virtio_gpu_reset(void *vdev);
static int virtio_gpu_cfgread(void *, int, int, uint32_t *);
static int virtio_gpu_cfgwrite(void *, int, int, uint32_t);
static void virtio_gpu_neg_features(void *, uint64_t);
static void virtio_gpu_set_status(void *, uint64_t);

static struct virtio_ops virtio_gpu_ops = {
	"virtio-gpu",			/* our name */
	VIRTIO_GPU_QNUM,		/* we currently support 2 virtqueues */
	sizeof(struct virtio_gpu_config),/* config reg size */
	virtio_gpu_reset,		/* reset */
	NULL,				/* device-wide qnotify */
	virtio_gpu_cfgread,		/* read PCI config */
	virtio_gpu_cfgwrite,		/* write PCI config */
	virtio_gpu_neg_features,	/* apply negotiated features */
	virtio_gpu_set_status,		/* called on guest set status */
};
static int virtio_gpu_device_cnt = 0;

static void
virtio_gpu_set_status(void *vdev, uint64_t status)
{
	struct virtio_gpu *gpu;

	pr_dbg("virtio-gpu setting deivce status 0x%x.\n", status);
	gpu = vdev;
	gpu->base.status = status;
}

static void
virtio_gpu_reset(void *vdev)
{
	struct virtio_gpu *gpu;
	struct virtio_gpu_resource_2d *r2d;

	pr_dbg("Resetting virtio-gpu device.\n");
	gpu = vdev;
	while (LIST_FIRST(&gpu->r2d_list)) {
		r2d = LIST_FIRST(&gpu->r2d_list);
		if (r2d) {
			if (r2d->image) {
				pixman_image_unref(r2d->image);
				r2d->image = NULL;
			}
			LIST_REMOVE(r2d, link);
			if (r2d->iov) {
				free(r2d->iov);
				r2d->iov = NULL;
			}
			free(r2d);
		}
	}
	LIST_INIT(&gpu->r2d_list);
	virtio_reset_dev(&gpu->base);
}

static int
virtio_gpu_cfgread(void *vdev, int offset, int size, uint32_t *retval)
{
	struct virtio_gpu *gpu;
	void *ptr;

	gpu = vdev;
	ptr = (uint8_t *)&gpu->cfg + offset;
	memcpy(retval, ptr, size);

	return 0;
}

static int
virtio_gpu_cfgwrite(void *vdev, int offset, int size, uint32_t value)
{
	struct virtio_gpu *gpu;
	void *ptr;

	gpu = vdev;
	ptr = (uint8_t *)&gpu->cfg + offset;
	if (offset == offsetof(struct virtio_gpu_config, events_clear)) {

		memcpy(ptr, &value, size);
		gpu->cfg.events_read &= ~value;
		gpu->cfg.events_clear &= ~value;
	}
	pr_err("%s: write to read-only regisiters.\n", __func__);

	return 0;
}

static void
virtio_gpu_neg_features(void *vdev, uint64_t negotiated_features)
{
	struct virtio_gpu *gpu;

	pr_dbg("virtio-gpu driver negotiated feature bits 0x%x.\n",
			negotiated_features);
	gpu = vdev;
	gpu->base.negotiated_caps = negotiated_features;
}

static void
virtio_gpu_update_resp_fence(struct virtio_gpu_ctrl_hdr *hdr,
		struct virtio_gpu_ctrl_hdr *resp)
{
	if ((hdr == NULL ) || (resp == NULL))
		return;

	if(hdr->flags & VIRTIO_GPU_FLAG_FENCE) {
		resp->flags |= VIRTIO_GPU_FLAG_FENCE;
		resp->fence_id = hdr->fence_id;
	}
}

static void
virtio_gpu_cmd_unspec(struct virtio_gpu_command *cmd)
{
	struct virtio_gpu_ctrl_hdr resp;

	pr_info("virtio-gpu: unspec commands received.\n");
	memset(&resp, 0, sizeof(resp));
	cmd->iolen = sizeof(resp);
	resp.type = VIRTIO_GPU_RESP_ERR_UNSPEC;
	virtio_gpu_update_resp_fence(&cmd->hdr, &resp);
	memcpy(cmd->iov[cmd->iovcnt - 1].iov_base, &resp, sizeof(resp));
}

static void
virtio_gpu_cmd_get_edid(struct virtio_gpu_command *cmd)
{
	struct virtio_gpu_get_edid req;
	struct virtio_gpu_resp_edid resp;
	struct virtio_gpu *gpu;

	gpu = cmd->gpu;
	memcpy(&req, cmd->iov[0].iov_base, sizeof(req));
	cmd->iolen = sizeof(resp);
	memset(&resp, 0, sizeof(resp));
	/* Only one EDID block is enough */
	resp.size = 128;
	virtio_gpu_update_resp_fence(&cmd->hdr, &resp.hdr);
	vdpy_get_edid(gpu->vdpy_handle, resp.edid, resp.size);
	memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
}

static void
virtio_gpu_cmd_get_display_info(struct virtio_gpu_command *cmd)
{
	struct virtio_gpu_resp_display_info resp;
	struct display_info info;
	struct virtio_gpu *gpu;

	gpu = cmd->gpu;
	cmd->iolen = sizeof(resp);
	memset(&resp, 0, sizeof(resp));
	vdpy_get_display_info(gpu->vdpy_handle, &info);
	resp.hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
	virtio_gpu_update_resp_fence(&cmd->hdr, &resp.hdr);
	resp.pmodes[0].enabled = 1;
	resp.pmodes[0].r.x = info.xoff;
	resp.pmodes[0].r.y = info.yoff;
	resp.pmodes[0].r.width = info.width;
	resp.pmodes[0].r.height = info.height;
	memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
}

static struct virtio_gpu_resource_2d *
virtio_gpu_find_resource_2d(struct virtio_gpu *gpu, uint32_t resource_id)
{
	struct virtio_gpu_resource_2d *r2d;

	LIST_FOREACH(r2d, &gpu->r2d_list, link) {
		if (r2d->resource_id == resource_id) {
			return r2d;
		}
	}

	return NULL;
}

static pixman_format_code_t
virtio_gpu_get_pixman_format(uint32_t format)
{
	switch (format) {
	case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
		pr_dbg("%s: format B8G8R8X8.\n", __func__);
		return PIXMAN_x8r8g8b8;
	case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
		pr_dbg("%s: format B8G8R8A8.\n", __func__);
		return PIXMAN_a8r8g8b8;
	case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
		pr_dbg("%s: format X8R8G8B8.\n", __func__);
		return PIXMAN_b8g8r8x8;
	case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
		pr_dbg("%s: format A8R8G8B8.\n", __func__);
		return PIXMAN_b8g8r8a8;
	case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
		pr_dbg("%s: format R8G8B8X8.\n", __func__);
		return PIXMAN_x8b8g8r8;
	case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
		pr_dbg("%s: format R8G8B8A8.\n", __func__);
		return PIXMAN_a8b8g8r8;
	case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
		pr_dbg("%s: format X8B8G8R8.\n", __func__);
		return PIXMAN_r8g8b8x8;
	case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
		pr_dbg("%s: format A8B8G8R8.\n", __func__);
		return PIXMAN_r8g8b8a8;
	default:
		return 0;
	}
}

static void
virtio_gpu_cmd_resource_create_2d(struct virtio_gpu_command *cmd)
{
	struct virtio_gpu_resource_create_2d req;
	struct virtio_gpu_ctrl_hdr resp;
	struct virtio_gpu_resource_2d *r2d;

	memcpy(&req, cmd->iov[0].iov_base, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	r2d = virtio_gpu_find_resource_2d(cmd->gpu, req.resource_id);
	if (r2d) {
		pr_dbg("%s: resource %d already exists.\n",
				__func__, req.resource_id);
		resp.type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
		goto response;
	}
	r2d = (struct virtio_gpu_resource_2d*)calloc(1, \
			sizeof(struct virtio_gpu_resource_2d));
	r2d->resource_id = req.resource_id;
	r2d->width = req.width;
	r2d->height = req.height;
	r2d->format = virtio_gpu_get_pixman_format(req.format);
	r2d->image = pixman_image_create_bits(
			r2d->format, r2d->width, r2d->height, NULL, 0);
	if (!r2d->image) {
		pr_err("%s: could not create resource %d (%d,%d).\n",
				__func__,
				r2d->resource_id,
				r2d->width,
				r2d->height);
		free(r2d);
		resp.type = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
	} else {
		resp.type = VIRTIO_GPU_RESP_OK_NODATA;
		LIST_INSERT_HEAD(&cmd->gpu->r2d_list, r2d, link);
	}

response:
	cmd->iolen = sizeof(resp);
	virtio_gpu_update_resp_fence(&cmd->hdr, &resp);
	memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
}

static void
virtio_gpu_cmd_resource_unref(struct virtio_gpu_command *cmd)
{
	struct virtio_gpu_resource_unref req;
	struct virtio_gpu_ctrl_hdr resp;
	struct virtio_gpu_resource_2d *r2d;

	memcpy(&req, cmd->iov[0].iov_base, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	r2d = virtio_gpu_find_resource_2d(cmd->gpu, req.resource_id);
	if (r2d) {
		if (r2d->image) {
			pixman_image_unref(r2d->image);
			r2d->image = NULL;
		}
		LIST_REMOVE(r2d, link);
		if (r2d->iov) {
			free(r2d->iov);
			r2d->iov = NULL;
		}
		free(r2d);
		resp.type = VIRTIO_GPU_RESP_OK_NODATA;
	} else {
		pr_err("%s: Illegal resource id %d\n", __func__, req.resource_id);
		resp.type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	}

	cmd->iolen = sizeof(resp);
	virtio_gpu_update_resp_fence(&cmd->hdr, &resp);
	memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
}

static void
virtio_gpu_cmd_resource_attach_backing(struct virtio_gpu_command *cmd)
{
	struct virtio_gpu_resource_attach_backing req;
	struct virtio_gpu_mem_entry *entries;
	struct virtio_gpu_resource_2d *r2d;
	struct virtio_gpu_ctrl_hdr resp;
	int i;
	uint8_t *pbuf;

	memcpy(&req, cmd->iov[0].iov_base, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	r2d = virtio_gpu_find_resource_2d(cmd->gpu, req.resource_id);
	if (r2d) {
		r2d->iov = malloc(req.nr_entries * sizeof(struct iovec));
		r2d->iovcnt = req.nr_entries;
		entries = malloc(req.nr_entries * sizeof(struct virtio_gpu_mem_entry));
		pbuf = (uint8_t*)entries;
		for (i = 1; i < (cmd->iovcnt - 1); i++) {
			memcpy(pbuf, cmd->iov[i].iov_base, cmd->iov[i].iov_len);
			pbuf += cmd->iov[i].iov_len;
		}
		for (i = 0; i < req.nr_entries; i++) {
			r2d->iov[i].iov_base = paddr_guest2host(
					cmd->gpu->base.dev->vmctx,
					entries[i].addr,
					entries[i].length);
			r2d->iov[i].iov_len = entries[i].length;
		}
		free(entries);
	} else {
		pr_err("%s: Illegal resource id %d\n", __func__, req.resource_id);
		resp.type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	}

	cmd->iolen = sizeof(resp);
	resp.type = VIRTIO_GPU_RESP_OK_NODATA;
	virtio_gpu_update_resp_fence(&cmd->hdr, &resp);
	memcpy(cmd->iov[cmd->iovcnt - 1].iov_base, &resp, sizeof(resp));
}

static void
virtio_gpu_cmd_resource_detach_backing(struct virtio_gpu_command *cmd)
{
	struct virtio_gpu_resource_detach_backing req;
	struct virtio_gpu_resource_2d *r2d;
	struct virtio_gpu_ctrl_hdr resp;

	memcpy(&req, cmd->iov[0].iov_base, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	r2d = virtio_gpu_find_resource_2d(cmd->gpu, req.resource_id);
	if (r2d && r2d->iov) {
		free(r2d->iov);
		r2d->iov = NULL;
	}

	cmd->iolen = sizeof(resp);
	resp.type = VIRTIO_GPU_RESP_OK_NODATA;
	virtio_gpu_update_resp_fence(&cmd->hdr, &resp);
	memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
}

static void
virtio_gpu_cmd_set_scanout(struct virtio_gpu_command *cmd)
{
	struct virtio_gpu_set_scanout req;
	struct virtio_gpu_resource_2d *r2d;
	struct virtio_gpu_ctrl_hdr resp;
	struct surface surf;
	struct virtio_gpu *gpu;

	gpu = cmd->gpu;
	memcpy(&req, cmd->iov[0].iov_base, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	virtio_gpu_update_resp_fence(&cmd->hdr, &resp);

	r2d = virtio_gpu_find_resource_2d(gpu, req.resource_id);
	if ((req.resource_id == 0) || (r2d == NULL)) {
		vdpy_surface_set(gpu->vdpy_handle, NULL);
		resp.type = VIRTIO_GPU_RESP_OK_NODATA;
		memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
		return;
	}
	if ((req.r.x > r2d->width) ||
	    (req.r.y > r2d->height) ||
	    (req.r.width > r2d->width) ||
	    (req.r.height > r2d->height) ||
	    (req.r.x + req.r.width) > (r2d->width) ||
	    (req.r.y + req.r.height) > (r2d->height)) {
		pr_err("%s: Scanout bound out of underlying resource.\n",
				__func__);
		resp.type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
	} else {
		pixman_image_ref(r2d->image);
		surf.pixel = pixman_image_get_data(r2d->image);
		surf.x = 0;
		surf.y = 0;
		surf.width = r2d->width;
		surf.height = r2d->height;
		surf.stride = pixman_image_get_stride(r2d->image);
		surf.surf_format = r2d->format;
		surf.surf_type = SURFACE_PIXMAN;
		vdpy_surface_set(gpu->vdpy_handle, &surf);
		pixman_image_unref(r2d->image);
		resp.type = VIRTIO_GPU_RESP_OK_NODATA;
	}

	cmd->iolen = sizeof(resp);
	memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
}

static void
virtio_gpu_cmd_transfer_to_host_2d(struct virtio_gpu_command *cmd)
{
	struct virtio_gpu_transfer_to_host_2d req;
	struct virtio_gpu_resource_2d *r2d;
	struct virtio_gpu_ctrl_hdr resp;
	uint32_t src_offset, dst_offset, stride, bpp, h;
	pixman_format_code_t format;
	void *img_data, *dst, *src;
	int i, done, bytes, total;
	int width, height;

	memcpy(&req, cmd->iov[0].iov_base, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	virtio_gpu_update_resp_fence(&cmd->hdr, &resp);

	r2d = virtio_gpu_find_resource_2d(cmd->gpu, req.resource_id);
	if (r2d == NULL) {
		pr_err("%s: Illegal resource id %d\n", __func__,
				req.resource_id);
		resp.type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
		memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
		return;
	}
	if ((req.r.x > r2d->width) ||
	    (req.r.y > r2d->height) ||
	    (req.r.width > r2d->width) ||
	    (req.r.height > r2d->height) ||
	    (req.r.x + req.r.width > r2d->width) ||
	    (req.r.y + req.r.height > r2d->height)) {
		pr_err("%s: transfer bounds outside resource.\n", __func__);
		resp.type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
	} else {
		pixman_image_ref(r2d->image);
		stride = pixman_image_get_stride(r2d->image);
		format = pixman_image_get_format(r2d->image);
		bpp = PIXMAN_FORMAT_BPP(format) / 8;
		img_data = pixman_image_get_data(r2d->image);
		width = (req.r.width < r2d->width) ? req.r.width : r2d->width;
		height = (req.r.height < r2d->height) ? req.r.height : r2d->height;
		for (h = 0; h < height; h++) {
			src_offset = req.offset + stride * h;
			dst_offset = (req.r.y + h) * stride + (req.r.x * bpp);
			dst = img_data + dst_offset;
			done = 0;
			total = width * bpp;
			for (i = 0; i < r2d->iovcnt; i++) {
				if ((r2d->iov[i].iov_base == 0) || (r2d->iov[i].iov_len == 0)) {
					continue;
				}

				if (src_offset < r2d->iov[i].iov_len) {
					src = r2d->iov[i].iov_base + src_offset;
					bytes = ((total - done) < (r2d->iov[i].iov_len - src_offset)) ?
						 (total - done) : (r2d->iov[i].iov_len - src_offset);
					memcpy((dst + done), src, bytes);
					src_offset = 0;
					done += bytes;
					if (done >= total) {
						break;
					}
				} else {
					src_offset -= r2d->iov[i].iov_len;
				}
			}
		}
		pixman_image_unref(r2d->image);
		resp.type = VIRTIO_GPU_RESP_OK_NODATA;
	}

	cmd->iolen = sizeof(resp);
	memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
}

static void
virtio_gpu_cmd_resource_flush(struct virtio_gpu_command *cmd)
{
	struct virtio_gpu_resource_flush req;
	struct virtio_gpu_ctrl_hdr resp;
	struct virtio_gpu_resource_2d *r2d;
	struct surface surf;
	struct virtio_gpu *gpu;

	gpu = cmd->gpu;
	memcpy(&req, cmd->iov[0].iov_base, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	virtio_gpu_update_resp_fence(&cmd->hdr, &resp);

	r2d = virtio_gpu_find_resource_2d(gpu, req.resource_id);
	if (r2d == NULL) {
		pr_err("%s: Illegal resource id %d\n", __func__,
				req.resource_id);
		resp.type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
		memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
		return;
	}
	pixman_image_ref(r2d->image);
	surf.pixel = pixman_image_get_data(r2d->image);
	surf.x = req.r.x;
	surf.y = req.r.y;
	surf.width = r2d->width;
	surf.height = r2d->height;
	surf.stride = pixman_image_get_stride(r2d->image);
	surf.surf_format = r2d->format;
	surf.surf_type = SURFACE_PIXMAN;
	vdpy_surface_update(gpu->vdpy_handle, &surf);
	pixman_image_unref(r2d->image);

	cmd->iolen = sizeof(resp);
	resp.type = VIRTIO_GPU_RESP_OK_NODATA;
	memcpy(cmd->iov[1].iov_base, &resp, sizeof(resp));
}

static void
virtio_gpu_ctrl_bh(void *data)
{
	struct virtio_gpu *vdev;
	struct virtio_vq_info *vq;
	struct virtio_gpu_command cmd;
	struct iovec iov[VIRTIO_GPU_MAXSEGS];
	uint16_t flags[VIRTIO_GPU_MAXSEGS];
	int n;
	uint16_t idx;

	vq = (struct virtio_vq_info *)data;
	vdev = (struct virtio_gpu *)(vq->base);
	cmd.gpu = vdev;
	cmd.iolen = 0;

	while (vq_has_descs(vq)) {
		n = vq_getchain(vq, &idx, iov, VIRTIO_GPU_MAXSEGS, flags);
		if (n < 0) {
			pr_err("virtio-gpu: invalid descriptors\n");
			return;
		}
		if (n == 0) {
			pr_err("virtio-gpu: get no available descriptors\n");
			return;
		}

		cmd.iovcnt = n;
		cmd.iov = iov;
		memcpy(&cmd.hdr, iov[0].iov_base,
			sizeof(struct virtio_gpu_ctrl_hdr));

		switch (cmd.hdr.type) {
		case VIRTIO_GPU_CMD_GET_EDID:
			virtio_gpu_cmd_get_edid(&cmd);
		case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
			virtio_gpu_cmd_get_display_info(&cmd);
			break;
		case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
			virtio_gpu_cmd_resource_create_2d(&cmd);
			break;
		case VIRTIO_GPU_CMD_RESOURCE_UNREF:
			virtio_gpu_cmd_resource_unref(&cmd);
			break;
		case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
			virtio_gpu_cmd_resource_attach_backing(&cmd);
			break;
		case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
			virtio_gpu_cmd_resource_detach_backing(&cmd);
			break;
		case VIRTIO_GPU_CMD_SET_SCANOUT:
			virtio_gpu_cmd_set_scanout(&cmd);
			break;
		case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
			virtio_gpu_cmd_transfer_to_host_2d(&cmd);
			break;
		case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
			virtio_gpu_cmd_resource_flush(&cmd);
			break;
		default:
			virtio_gpu_cmd_unspec(&cmd);
			break;
		}

		vq_relchain(vq, idx, cmd.iolen); /* Release the chain */
	}
	vq_endchains(vq, 1);	/* Generate interrupt if appropriate. */
}

static void
virtio_gpu_notify_controlq(void *vdev, struct virtio_vq_info *vq)
{
	struct virtio_gpu *gpu;

	gpu = (struct virtio_gpu *)vdev;
	vdpy_submit_bh(gpu->vdpy_handle, &gpu->ctrl_bh);
}

static void
virtio_gpu_notify_cursorq(void *vdev, struct virtio_vq_info *vq)
{
	struct virtio_gpu_command cmd;
	struct virtio_gpu_ctrl_hdr hdr;
	struct iovec iov[VIRTIO_GPU_MAXSEGS];
	int n;
	uint16_t idx;

	cmd.gpu = vdev;
	cmd.iolen = 0;
	while (vq_has_descs(vq)) {
		n = vq_getchain(vq, &idx, iov, VIRTIO_GPU_MAXSEGS, NULL);
		if (n < 0) {
			pr_err("virtio-gpu: invalid descriptors\n");
			return;
		}
		if (n == 0) {
			pr_err("virtio-gpu: get no available descriptors\n");
			return;
		}
		cmd.iovcnt = n;
		cmd.iov = iov;
		memcpy(&hdr, iov[0].iov_base, sizeof(hdr));
		switch (hdr.type) {
		default:
			virtio_gpu_cmd_unspec(&cmd);
			break;
		}

		vq_relchain(vq, idx, cmd.iolen); /* Release the chain */
	}
	vq_endchains(vq, 1);	/* Generate interrupt if appropriate. */
}

static int
virtio_gpu_init(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_gpu *gpu;
	pthread_mutexattr_t attr;
	int rc = 0;

	if (virtio_gpu_device_cnt) {
		pr_err("%s: only 1 virtio-gpu device can be created.\n", __func__);
		return -1;
	}
	virtio_gpu_device_cnt++;

	/* allocate the virtio-gpu device */
	gpu = calloc(1, sizeof(struct virtio_gpu));
	if (!gpu) {
		pr_err("%s: out of memory\n", __func__);
		return -1;
	}

	/* init mutex attribute properly to avoid deadlock */
	rc = pthread_mutexattr_init(&attr);
	if (rc) {
		pr_err("%s: mutexattr init failed with erro %d!\n",
			       __func__, rc);
		return rc;
	}
	rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	if (rc) {
		pr_err("%s: mutexattr_settype failed with error %d!\n",
			       __func__, rc);
		return rc;
	}
	rc = pthread_mutex_init(&gpu->mtx, &attr);
	if (rc) {
		pr_err("%s: pthread_mutex_init failed with error %d!\n",
			       __func__,rc);
		return rc;
	}

	/* register the virtio_gpu_ops to virtio framework */
	virtio_linkup(&gpu->base,
			&virtio_gpu_ops,
			gpu,
			dev,
			gpu->vq,
			BACKEND_VBSU);

	gpu->base.mtx = &gpu->mtx;
	gpu->base.device_caps = VIRTIO_GPU_S_HOSTCAPS;

	/* set queue size */
	gpu->vq[VIRTIO_GPU_CONTROLQ].qsize = VIRTIO_GPU_RINGSZ;
	gpu->vq[VIRTIO_GPU_CONTROLQ].notify = virtio_gpu_notify_controlq;
	gpu->vq[VIRTIO_GPU_CURSORQ].qsize = VIRTIO_GPU_RINGSZ;
	gpu->vq[VIRTIO_GPU_CURSORQ].notify = virtio_gpu_notify_cursorq;

	/* Initialize the ctrl bh_task */
	gpu->ctrl_bh.task_cb = virtio_gpu_ctrl_bh;
	gpu->ctrl_bh.data = &gpu->vq[VIRTIO_GPU_CONTROLQ];

	/* prepare the config space */
	gpu->cfg.events_read = 0;
	gpu->cfg.events_clear = 0;
	gpu->cfg.num_scanouts = 1;
	gpu->cfg.num_capsets = 0;

	/* config the device id and vendor id according to spec */
	pci_set_cfgdata16(dev, PCIR_DEVICE, VIRTIO_DEV_GPU);
	pci_set_cfgdata16(dev, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata16(dev, PCIR_REVID, 1);
	pci_set_cfgdata8(dev, PCIR_CLASS, PCIC_DISPLAY);
	pci_set_cfgdata8(dev, PCIR_SUBCLASS, PCIS_DISPLAY_OTHER);
	pci_set_cfgdata16(dev, PCIR_SUBDEV_0, VIRTIO_TYPE_GPU);
	pci_set_cfgdata16(dev, PCIR_SUBVEND_0, VIRTIO_VENDOR);

	LIST_INIT(&gpu->r2d_list);

	/*** PCI Config BARs setup ***/
	rc = virtio_interrupt_init(&gpu->base, virtio_uses_msix());
	if (rc) {
		pr_err("%s, interrupt_init failed.\n", __func__);
		return rc;
	}
	rc = virtio_set_modern_bar(&gpu->base, true);
	if (rc) {
		pr_err("%s, set modern bar failed.\n", __func__);
		return rc;
	}
	gpu->vdpy_handle = vdpy_init();

	return 0;
}

static void
virtio_gpu_deinit(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct virtio_gpu *gpu;
	struct virtio_gpu_resource_2d *r2d;

	gpu = (struct virtio_gpu *)dev->arg;

	while (LIST_FIRST(&gpu->r2d_list)) {
		r2d = LIST_FIRST(&gpu->r2d_list);
		if (r2d) {
			if (r2d->image) {
				pixman_image_unref(r2d->image);
				r2d->image = NULL;
			}
			LIST_REMOVE(r2d, link);
			if (r2d->iov) {
				free(r2d->iov);
				r2d->iov = NULL;
			}
			free(r2d);
		}
	}

	if (gpu) {
		pthread_mutex_destroy(&gpu->mtx);
		free(gpu);
	}
	virtio_gpu_device_cnt--;
	vdpy_deinit(gpu->vdpy_handle);
}

static void
virtio_gpu_write(struct vmctx *ctx, int vcpu, struct pci_vdev *dev,
		int baridx, uint64_t offset, int size, uint64_t value)
{
	virtio_pci_write(ctx, vcpu, dev, baridx, offset, size, value);
}

static uint64_t
virtio_gpu_read(struct vmctx *ctx, int vcpu, struct pci_vdev *dev,
		int baridx, uint64_t offset, int size)
{
	return virtio_pci_read(ctx, vcpu, dev, baridx, offset, size);
}


struct pci_vdev_ops pci_ops_virtio_gpu = {
	.class_name     = "virtio-gpu",
	.vdev_init      = virtio_gpu_init,
	.vdev_deinit    = virtio_gpu_deinit,
	.vdev_barwrite	= virtio_gpu_write,
	.vdev_barread	= virtio_gpu_read
};
DEFINE_PCI_DEVTYPE(pci_ops_virtio_gpu);
