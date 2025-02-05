// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe I/O command implementation.
 * Copyright (c) 2015-2016 HGST, a Western Digital Company.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/blkdev.h>
#include <linux/nvme-tcp.h>
#include <linux/module.h>
#include "nvmet.h"
#include "bpfof.h"

struct hugepage_pool_entry {
	struct page *page;
	bool in_use;
};

#define HUGEPAGE_POOL_SIZE 100

struct hugepage_pool_entry *hugepage_pool;
int hugepage_pool_size;
int num_cpus;

// TODO: Destroy hugepage pool on exit

void hugepage_pool_init_for_all_cpus(void) {
	// Get number of cpus
	int i;
	num_cpus = num_online_cpus();
	hugepage_pool = kmalloc_array(num_cpus * HUGEPAGE_POOL_SIZE, sizeof(struct hugepage_pool_entry),GFP_KERNEL);
	hugepage_pool_size = num_cpus * HUGEPAGE_POOL_SIZE;

	for (i = 0; i < hugepage_pool_size; i++) {
		hugepage_pool[i].page = alloc_pages(GFP_KERNEL, 9);
		hugepage_pool[i].in_use = false;
	}
}
EXPORT_SYMBOL(hugepage_pool_init_for_all_cpus);

struct page* get_available_hugepage(void) {
	int i;
	rcu_read_lock();
	for (i = smp_processor_id()*HUGEPAGE_POOL_SIZE; i < smp_processor_id()*HUGEPAGE_POOL_SIZE + HUGEPAGE_POOL_SIZE; i++) {
		if (!hugepage_pool[i].in_use) {
			hugepage_pool[i].in_use = true;
			rcu_read_unlock();
			return hugepage_pool[i].page;
		}
	}

	rcu_read_unlock();
	pr_warn("WARNING: No available hugepage in pool\n");
	return NULL;
}

int put_hugepage(struct page* hugepage) {
	int i;
	rcu_read_lock();
	for (i = smp_processor_id()*HUGEPAGE_POOL_SIZE; i < smp_processor_id()*HUGEPAGE_POOL_SIZE + HUGEPAGE_POOL_SIZE; i++) {
		if (hugepage_pool[i].page == hugepage) {
			if (hugepage_pool[i].in_use == false) {
				break;
			}
			hugepage_pool[i].in_use = false;
			rcu_read_unlock();
			return 0;
		}
	}
	rcu_read_unlock();
	return -1;
}


void nvmet_bdev_set_limits(struct block_device *bdev, struct nvme_id_ns *id) {
	const struct queue_limits *ql = &bdev_get_queue(bdev)->limits;
	/* Number of logical blocks per physical block. */
	const u32 lpp = ql->physical_block_size / ql->logical_block_size;
	/* Logical blocks per physical block, 0's based. */
	const __le16 lpp0b = to0based(lpp);

	/*
	 * For NVMe 1.2 and later, bit 1 indicates that the fields NAWUN,
	 * NAWUPF, and NACWU are defined for this namespace and should be
	 * used by the host for this namespace instead of the AWUN, AWUPF,
	 * and ACWU fields in the Identify Controller data structure. If
	 * any of these fields are zero that means that the corresponding
	 * field from the identify controller data structure should be used.
	 */
	id->nsfeat |= 1 << 1;
	id->nawun = lpp0b;
	id->nawupf = lpp0b;
	id->nacwu = lpp0b;

	/*
	 * Bit 4 indicates that the fields NPWG, NPWA, NPDG, NPDA, and
	 * NOWS are defined for this namespace and should be used by
	 * the host for I/O optimization.
	 */
	id->nsfeat |= 1 << 4;
	/* NPWG = Namespace Preferred Write Granularity. 0's based */
	id->npwg = lpp0b;
	/* NPWA = Namespace Preferred Write Alignment. 0's based */
	id->npwa = id->npwg;
	/* NPDG = Namespace Preferred Deallocate Granularity. 0's based */
	id->npdg = to0based(ql->discard_granularity / ql->logical_block_size);
	/* NPDG = Namespace Preferred Deallocate Alignment */
	id->npda = id->npdg;
	/* NOWS = Namespace Optimal Write Size */
	id->nows = to0based(ql->io_opt / ql->logical_block_size);
}

static void nvmet_bdev_ns_enable_integrity(struct nvmet_ns *ns) {
	struct blk_integrity *bi = bdev_get_integrity(ns->bdev);

	if (bi) {
		ns->metadata_size = bi->tuple_size;
		if (bi->profile == &t10_pi_type1_crc)
			ns->pi_type = NVME_NS_DPS_PI_TYPE1;
		else if (bi->profile == &t10_pi_type3_crc)
			ns->pi_type = NVME_NS_DPS_PI_TYPE3;
		else
			/* Unsupported metadata type */
			ns->metadata_size = 0;
	}
}

int nvmet_bdev_ns_enable(struct nvmet_ns *ns) {
	int ret;

	ns->bdev = blkdev_get_by_path(ns->device_path, FMODE_READ | FMODE_WRITE,
								  NULL);
	if (IS_ERR(ns->bdev)) {
		ret = PTR_ERR(ns->bdev);
		if (ret != -ENOTBLK) {
			pr_err("failed to open block device %s: (%ld)\n", ns->device_path,
				   PTR_ERR(ns->bdev));
		}
		ns->bdev = NULL;
		return ret;
	}
	ns->size = i_size_read(ns->bdev->bd_inode);
	ns->blksize_shift = blksize_bits(bdev_logical_block_size(ns->bdev));

	ns->pi_type = 0;
	ns->metadata_size = 0;
	if (IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY_T10))
		nvmet_bdev_ns_enable_integrity(ns);

	return 0;
}

void nvmet_bdev_ns_disable(struct nvmet_ns *ns) {
	if (ns->bdev) {
		blkdev_put(ns->bdev, FMODE_WRITE | FMODE_READ);
		ns->bdev = NULL;
	}
}

void nvmet_bdev_ns_revalidate(struct nvmet_ns *ns) {
	ns->size = i_size_read(ns->bdev->bd_inode);
}

static u16 blk_to_nvme_status(struct nvmet_req *req, blk_status_t blk_sts) {
	u16 status = NVME_SC_SUCCESS;

	if (likely(blk_sts == BLK_STS_OK)) return status;
	/*
	 * Right now there exists M : 1 mapping between block layer error
	 * to the NVMe status code (see nvme_error_status()). For consistency,
	 * when we reverse map we use most appropriate NVMe Status code from
	 * the group of the NVMe staus codes used in the nvme_error_status().
	 */
	switch (blk_sts) {
		case BLK_STS_NOSPC:
			status = NVME_SC_CAP_EXCEEDED | NVME_SC_DNR;
			req->error_loc = offsetof(struct nvme_rw_command, length);
			break;
		case BLK_STS_TARGET:
			status = NVME_SC_LBA_RANGE | NVME_SC_DNR;
			req->error_loc = offsetof(struct nvme_rw_command, slba);
			break;
		case BLK_STS_NOTSUPP:
			req->error_loc = offsetof(struct nvme_common_command, opcode);
			switch (req->cmd->common.opcode) {
				case nvme_cmd_dsm:
				case nvme_cmd_write_zeroes:
					status = NVME_SC_ONCS_NOT_SUPPORTED | NVME_SC_DNR;
					break;
				default:
					status = NVME_SC_INVALID_OPCODE | NVME_SC_DNR;
			}
			break;
		case BLK_STS_MEDIUM:
			status = NVME_SC_ACCESS_DENIED;
			req->error_loc = offsetof(struct nvme_rw_command, nsid);
			break;
		case BLK_STS_IOERR:
		default:
			status = NVME_SC_INTERNAL | NVME_SC_DNR;
			req->error_loc = offsetof(struct nvme_common_command, opcode);
	}

	switch (req->cmd->common.opcode) {
		case nvme_cmd_read:
		case nvme_cmd_write:
			req->error_slba = le64_to_cpu(req->cmd->rw.slba);
			break;
		case nvme_cmd_write_zeroes:
			req->error_slba = le64_to_cpu(req->cmd->write_zeroes.slba);
			break;
		default:
			req->error_slba = 0;
	}
	return status;
}

bool nvmeof_xrp_use_hugepages = false;
EXPORT_SYMBOL(nvmeof_xrp_use_hugepages);

static void nvmet_bio_done(struct bio *bio) {
	struct nvmet_req *req = bio->bi_private;

	// If XRP was used for this request, free the bio buffer.
	if (bio->xrp_enabled) {
		// DEBUG INFO
		// char *scratch_buffer_addr;
		// struct page *data_page;
		// data_page = bio_page(bio);
		// pr_debug("nvmeof_xrp: DEBUG: IO ended, got page %px)\n", data_page);
		// pr_debug("nvmeof_xrp: DEBUG: IO ended, got from struct access %px)\n",
		// bio->bi_io_vec->bv_page); scratch_buffer_addr =
		// page_to_virt(bio->xrp_scratch_page); pr_debug("nvmeof_xrp: IO ended,
		// Scratch buffer first bytes: %x %x %x %x\n", 	scratch_buffer_addr[0],
		// scratch_buffer_addr[1], 	scratch_buffer_addr[2],
		// scratch_buffer_addr[3]);

		if (bio->bi_io_vec->bv_page != NULL) {
			if (!nvmeof_xrp_use_hugepages)
				__free_page(bio->bi_io_vec->bv_page);
			else {
				int ret = put_hugepage(bio->bi_io_vec->bv_page);
				if (ret != 0)
					pr_err("nvmeof_xrp: ERROR: put_hugepage failed with error %d", ret);
			}
		}
	}

	nvmet_req_complete(req, blk_to_nvme_status(req, bio->bi_status));
	if (bio != &req->b.inline_bio) bio_put(bio);
}

#ifdef CONFIG_BLK_DEV_INTEGRITY
static int nvmet_bdev_alloc_bip(struct nvmet_req *req, struct bio *bio,
								struct sg_mapping_iter *miter) {
	struct blk_integrity *bi;
	struct bio_integrity_payload *bip;
	struct block_device *bdev = req->ns->bdev;
	int rc;
	size_t resid, len;

	bi = bdev_get_integrity(bdev);
	if (unlikely(!bi)) {
		pr_err("Unable to locate bio_integrity\n");
		return -ENODEV;
	}

	bip = bio_integrity_alloc(bio, GFP_NOIO,
							  bio_max_segs(req->metadata_sg_cnt));
	if (IS_ERR(bip)) {
		pr_err("Unable to allocate bio_integrity_payload\n");
		return PTR_ERR(bip);
	}

	bip->bip_iter.bi_size = bio_integrity_bytes(bi, bio_sectors(bio));
	/* virtual start sector must be in integrity interval units */
	bip_set_seed(bip,
				 bio->bi_iter.bi_sector >> (bi->interval_exp - SECTOR_SHIFT));

	resid = bip->bip_iter.bi_size;
	while (resid > 0 && sg_miter_next(miter)) {
		len = min_t(size_t, miter->length, resid);
		rc = bio_integrity_add_page(bio, miter->page, len,
									offset_in_page(miter->addr));
		if (unlikely(rc != len)) {
			pr_err("bio_integrity_add_page() failed; %d\n", rc);
			sg_miter_stop(miter);
			return -ENOMEM;
		}

		resid -= len;
		if (len < miter->length) miter->consumed -= miter->length - len;
	}
	sg_miter_stop(miter);

	return 0;
}
#else
static int nvmet_bdev_alloc_bip(struct nvmet_req *req, struct bio *bio,
								struct sg_mapping_iter *miter) {
	return -EINVAL;
}
#endif /* CONFIG_BLK_DEV_INTEGRITY */

int (*driver_get_nvmeof_xrp_info)(
	bool *xrp_enabled,
	struct bpf_prog **xrp_prog,
	struct bpfof_fd_info *bpfof_fd_info_arr,
	struct xrp_fd_info *xrp_fd_info_arr,
	size_t *xrp_fd_count) = NULL;
EXPORT_SYMBOL(driver_get_nvmeof_xrp_info);


static void nvmet_bdev_execute_rw(struct nvmet_req *req) {
	unsigned int sg_cnt = req->sg_cnt;
	struct bio *bio;
	struct scatterlist *sg;
	struct blk_plug plug;
	sector_t sector;
	int op, i, rc;
	struct sg_mapping_iter prot_miter;
	unsigned int iter_flags;
	unsigned int total_len = nvmet_rw_data_len(req) + req->metadata_len;

	pr_debug("nvmet_rw_data_len: %u, transfer len: %u, sg_cnt: %d\n",
			nvmet_rw_data_len(req), req->transfer_len, req->sg_cnt);
	if (!nvmet_check_transfer_len(req, total_len))
		return;

	if (!req->sg_cnt) {
		nvmet_req_complete(req, 0);
		return;
	}

	if (req->cmd->rw.opcode == nvme_cmd_write) {
		op = REQ_OP_WRITE | REQ_SYNC | REQ_IDLE;
		if (req->cmd->rw.control & cpu_to_le16(NVME_RW_FUA)) op |= REQ_FUA;
		iter_flags = SG_MITER_TO_SG;
	} else {
		// xrp_read will get REQ_OP_READ
		op = REQ_OP_READ;
		iter_flags = SG_MITER_FROM_SG;
	}

	if (is_pci_p2pdma_page(sg_page(req->sg))) op |= REQ_NOMERGE;

	sector = nvmet_lba_to_sect(req->ns, req->cmd->rw.slba);

	if (req->transfer_len <= NVMET_MAX_INLINE_DATA_LEN) {
		pr_debug("bio created from the inline data\n");
		bio = &req->b.inline_bio;
		bio_init(bio, req->inline_bvec, ARRAY_SIZE(req->inline_bvec));
	} else {
		bio = bio_alloc(GFP_KERNEL, bio_max_segs(sg_cnt));
	}
	bio_set_dev(bio, req->ns->bdev);
	bio->bi_iter.bi_sector = sector;
	bio->bi_private = req;
	bio->bi_end_io = nvmet_bio_done;
	bio->bi_opf = op;

	blk_start_plug(&plug);
	if (req->metadata_len)
		sg_miter_start(&prot_miter, req->metadata_sg, req->metadata_sg_cnt,
					   iter_flags);

	// xrp_metadata_target
	if (req->cmd->rw.opcode == nvme_cmd_xrp_read) {
		int ret;
		bool xrp_enabled;
		struct bpf_prog *xrp_prog;


		pr_debug("driver_get_nvmeof_xrp_info addr: %p\n", driver_get_nvmeof_xrp_info);
		if (driver_get_nvmeof_xrp_info == NULL) {
			pr_err("nvmeof_xrp: driver_get_nvmeof_xrp_info is NULL\n");
			bio_io_error(bio);
			return;
		}
		// Get scratch page from request scatter-gather list.
		bio->xrp_scratch_page = sg_page(req->sg);
		// Print the first bytes of the scratch buffer page
		char *scratch_buffer_addr;
		scratch_buffer_addr = page_address(bio->xrp_scratch_page);
		print_hex_dump_bytes("nvmeof_xrp: Scratch buffer first 512 bytes: ",
							 DUMP_PREFIX_NONE, scratch_buffer_addr, 512);
		print_hex_dump_bytes("nvmeof_xrp: Scratch buffer last 512 bytes: ",
				DUMP_PREFIX_NONE, scratch_buffer_addr + 4096-512-1, 512);


		struct bpfof_cmd_config bpfof_cmd_config;
		ret = deserialize_bpfof_cmd_config(scratch_buffer_addr + PAGE_SIZE - 1 - sizeof(struct bpfof_cmd_config),
				sizeof(struct bpfof_cmd_config), &bpfof_cmd_config);
		if (ret < 0) {
			pr_err("nvmeof_xrp: Error trying to deserialize XRP command config. Error code: '%d'\n", ret);
			bio_io_error(bio);
			return;
		}
		ret = driver_get_nvmeof_xrp_info(&xrp_enabled, &xrp_prog,
				bpfof_cmd_config.bpfof_fd_info_arr,
				bio->xrp_fd_info_arr, &bio->xrp_fd_count);
		if (ret) {
			pr_warn(
				"nvmeof_xrp: Error trying to get NVMEoF XRP info. Error"
				" code: '%d'\n",
				ret);
			bio_io_error(bio);
			return;
		}
		if (!xrp_enabled) {
			pr_err(
				"nvmeof_xrp: XRP command but driver returned not enabled.\n");
			bio_io_error(bio);
			return;
		}
		pr_debug("nvmeof_xrp: Enabled for NVMEoF/TCP request.\n");
		pr_debug("nvmeof_xrp: Request length: %lu.\n", req->transfer_len);

		bio->xrp_count = 1;
		bio->xrp_enabled = true;
		bio->xrp_cur_fd = bio->xrp_fd_info_arr[0].fd;
		bio->xrp_bpf_prog = xrp_prog;
		bio->xrp_original_bi_io_vec = NULL;
		bio->xrp_original_bi_max_vecs = 0;
		memset(&bio->xrp_bio_vec, 0, sizeof(struct bio_vec));
		memset(&bio->xrp_original_bi_iter, 0, sizeof(struct bvec_iter));
		// If this is an XRP request, the scatter-gather list contains the
		// scratch buffer. For the IO, we want to create a separate buffer.
		int xrp_read_length = bpfof_cmd_config.data_buffer_size;
		pr_debug("nvmeof_xrp: Data buffer size: %d\n", xrp_read_length);
		// TODO: Support bigger data buffers.
		struct page *data_page = NULL;
		if (!nvmeof_xrp_use_hugepages)
			data_page = alloc_page(GFP_ATOMIC);
		else{
			data_page = get_available_hugepage();
			if (data_page == NULL){
				pr_err("nvmeof_xrp: Failed to allocate hugepages.\n");
				bio_io_error(bio);
				return;
			}
		}
		if (xrp_read_length > (1<<21UL)) {
			pr_err("nvmeof_xrp: Data buffer size is larger than hugepage size.\n");
			bio_io_error(bio);
			return;
		}
		pr_debug("nvmeof_xrp: Allocated data page at address: %px\n",
				 data_page);
		if (!xrp_read_length) {
			// super-duper hacky recovery way.
			// for some yet undebugged reason we end up reading a zero length data buffer from the scratch buffer settings
			// truly bizarre....
			// bio_add_page(bio, data_page, 4096, 0);
			pr_err("nvmeof_xrp: Data buffer size is 0.\n");
			// bio_io_error(bio);
			goto no_xrp;
			return;
		}

		if (bio_add_page(bio, data_page, xrp_read_length, 0) !=
			xrp_read_length) {
			pr_err("nvmeof_xrp: Failed to add data buffer to bio.\n");
			bio_io_error(bio);
			return;
		}
	} else {
	no_xrp:
		pr_debug("nvmeof_xrp: XRP disabled for this request.\n");
		bio->xrp_enabled = false;

		for_each_sg(req->sg, sg, req->sg_cnt, i) {
			while (bio_add_page(bio, sg_page(sg), sg->length, sg->offset) !=
				   sg->length) {
				struct bio *prev = bio;

				if (req->metadata_len) {
					rc = nvmet_bdev_alloc_bip(req, bio, &prot_miter);
					if (unlikely(rc)) {
						bio_io_error(bio);
						return;
					}
				}

				bio = bio_alloc(GFP_KERNEL, bio_max_segs(sg_cnt));
				bio_set_dev(bio, req->ns->bdev);
				bio->bi_iter.bi_sector = sector;
				bio->bi_opf = op;

				bio_chain(bio, prev);
				submit_bio(prev);
			}

			sector += sg->length >> 9;
			sg_cnt--;
		}
	}

	if (req->metadata_len) {
		rc = nvmet_bdev_alloc_bip(req, bio, &prot_miter);
		if (unlikely(rc)) {
			bio_io_error(bio);
			return;
		}
	}

	submit_bio(bio);
	blk_finish_plug(&plug);
}

static void nvmet_bdev_execute_flush(struct nvmet_req *req) {
	struct bio *bio = &req->b.inline_bio;

	if (!nvmet_check_transfer_len(req, 0)) return;

	bio_init(bio, req->inline_bvec, ARRAY_SIZE(req->inline_bvec));
	bio_set_dev(bio, req->ns->bdev);
	bio->bi_private = req;
	bio->bi_end_io = nvmet_bio_done;
	bio->bi_opf = REQ_OP_WRITE | REQ_PREFLUSH;

	submit_bio(bio);
}

u16 nvmet_bdev_flush(struct nvmet_req *req) {
	if (blkdev_issue_flush(req->ns->bdev))
		return NVME_SC_INTERNAL | NVME_SC_DNR;
	return 0;
}

static u16 nvmet_bdev_discard_range(struct nvmet_req *req,
									struct nvme_dsm_range *range,
									struct bio **bio) {
	struct nvmet_ns *ns = req->ns;
	int ret;

	ret = __blkdev_issue_discard(
		ns->bdev, nvmet_lba_to_sect(ns, range->slba),
		le32_to_cpu(range->nlb) << (ns->blksize_shift - 9), GFP_KERNEL, 0, bio);
	if (ret && ret != -EOPNOTSUPP) {
		req->error_slba = le64_to_cpu(range->slba);
		return errno_to_nvme_status(req, ret);
	}
	return NVME_SC_SUCCESS;
}

static void nvmet_bdev_execute_discard(struct nvmet_req *req) {
	struct nvme_dsm_range range;
	struct bio *bio = NULL;
	int i;
	u16 status;

	for (i = 0; i <= le32_to_cpu(req->cmd->dsm.nr); i++) {
		status = nvmet_copy_from_sgl(req, i * sizeof(range), &range,
									 sizeof(range));
		if (status) break;

		status = nvmet_bdev_discard_range(req, &range, &bio);
		if (status) break;
	}

	if (bio) {
		bio->bi_private = req;
		bio->bi_end_io = nvmet_bio_done;
		if (status)
			bio_io_error(bio);
		else
			submit_bio(bio);
	} else {
		nvmet_req_complete(req, status);
	}
}

static void nvmet_bdev_execute_dsm(struct nvmet_req *req) {
	if (!nvmet_check_data_len_lte(req, nvmet_dsm_len(req))) return;

	switch (le32_to_cpu(req->cmd->dsm.attributes)) {
		case NVME_DSMGMT_AD:
			nvmet_bdev_execute_discard(req);
			return;
		case NVME_DSMGMT_IDR:
		case NVME_DSMGMT_IDW:
		default:
			/* Not supported yet */
			nvmet_req_complete(req, 0);
			return;
	}
}

static void nvmet_bdev_execute_write_zeroes(struct nvmet_req *req) {
	struct nvme_write_zeroes_cmd *write_zeroes = &req->cmd->write_zeroes;
	struct bio *bio = NULL;
	sector_t sector;
	sector_t nr_sector;
	int ret;

	if (!nvmet_check_transfer_len(req, 0)) return;

	sector = nvmet_lba_to_sect(req->ns, write_zeroes->slba);
	nr_sector = (((sector_t)le16_to_cpu(write_zeroes->length) + 1)
				 << (req->ns->blksize_shift - 9));

	ret = __blkdev_issue_zeroout(req->ns->bdev, sector, nr_sector, GFP_KERNEL,
								 &bio, 0);
	if (bio) {
		bio->bi_private = req;
		bio->bi_end_io = nvmet_bio_done;
		submit_bio(bio);
	} else {
		nvmet_req_complete(req, errno_to_nvme_status(req, ret));
	}
}

u16 nvmet_bdev_parse_io_cmd(struct nvmet_req *req) {
	struct nvme_command *cmd = req->cmd;

	switch (cmd->common.opcode) {
		case nvme_cmd_read:
		case nvme_cmd_write:
		case nvme_cmd_xrp_read:
			req->execute = nvmet_bdev_execute_rw;
			if (req->sq->ctrl->pi_support && nvmet_ns_has_pi(req->ns))
				req->metadata_len = nvmet_rw_metadata_len(req);
			return 0;
		case nvme_cmd_flush:
			req->execute = nvmet_bdev_execute_flush;
			return 0;
		case nvme_cmd_dsm:
			req->execute = nvmet_bdev_execute_dsm;
			return 0;
		case nvme_cmd_write_zeroes:
			req->execute = nvmet_bdev_execute_write_zeroes;
			return 0;
		default:
			return nvmet_report_invalid_opcode(req);
	}
}
