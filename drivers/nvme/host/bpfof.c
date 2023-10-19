#include <linux/export.h>
#include "bpfof.h"

bool (*driver_nvmeof_xrp_mapping_synced)(struct xrp_fd_info *xrp_fd_info_arr, size_t xrp_fd_count) = NULL;
EXPORT_SYMBOL(driver_nvmeof_xrp_mapping_synced);
