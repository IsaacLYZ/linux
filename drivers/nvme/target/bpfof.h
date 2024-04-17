#ifndef _NVME_TARGET_BPFOF_H
#define _NVME_TARGET_BPFOF_H


#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/nvme.h>

// Deserialization function
inline int deserialize_bpfof_cmd_config(char *buffer, size_t buffer_size, struct bpfof_cmd_config *config) {
	if (buffer_size < sizeof(struct bpfof_cmd_config)) {
		return -1;
	}
	memcpy(config, buffer, sizeof(struct bpfof_cmd_config));
	if (config->data_buffer_size == 0) {
		pr_warn("xrp_nvmeof: Got a zero-length read request\n");
		return -1;
	}
	return 0;
}

#endif // _NVME_TARGET_BPFOF_H