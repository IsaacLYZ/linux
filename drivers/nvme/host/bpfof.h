#ifndef _NVME_HOST_BPFOF_H
#define _NVME_HOST_BPFOF_H


#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/nvme.h>

extern bool (*driver_nvmeof_xrp_mapping_synced)(struct xrp_fd_info *xrp_fd_info_arr, size_t xrp_fd_count);

// Serialization function
inline int serialize_bpfof_cmd_config(struct request *rq, char *buffer, size_t buffer_size) {
	int i;
	struct bio *bio = rq->bio;
	if (buffer_size < sizeof(struct bpfof_cmd_config)) {
		return -1;
	}
	struct bpfof_cmd_config tmp = {
		.data_buffer_size = bio->bpfof_data_buffer_count,
	};
	if (bio->bpfof_data_buffer_count == 0) {
		pr_warn("xrp_nvmeof: Got a zero-length read request\n");
		return -1;
	}
	for (i = 0; i < bio->xrp_fd_count; i++) {
		tmp.bpfof_fd_info_arr[i].fd = bio->xrp_fd_info_arr[i].fd;
		tmp.bpfof_fd_info_arr[i].inode_identifier = bio->xrp_fd_info_arr[i].inode->i_ino;
	}
	for (i = bio->xrp_fd_count; i < 10; i++) {
		tmp.bpfof_fd_info_arr[i].fd = 0;
		tmp.bpfof_fd_info_arr[i].inode_identifier = 0;
	}
	pr_debug("xrp_nvmeof: XRP read request, data_len: %d, blk_rq_payload_bytes(rq): %d, fd %d\n",
		tmp.data_buffer_size, tmp.data_buffer_size, tmp.bpfof_fd_info_arr[0].fd);

	memcpy(buffer, &tmp, sizeof(struct bpfof_cmd_config));
	return 0;
}

#endif // _NVME_HOST_BPFOF_H