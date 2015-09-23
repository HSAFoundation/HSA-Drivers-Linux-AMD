/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/fdtable.h>
#include <linux/uaccess.h>
#include <linux/firmware.h>
#include <drm/drmP.h>
#include "amdgpu.h"
#include "amdgpu_amdkfd.h"
#include "cikd.h"
#include "cik_sdma.h"
#include "amdgpu_ucode.h"
#include "gca/gfx_7_2_d.h"
#include "gca/gfx_7_2_enum.h"
#include "gca/gfx_7_2_sh_mask.h"
#include "oss/oss_2_0_d.h"
#include "oss/oss_2_0_sh_mask.h"
#include "gmc/gmc_7_1_d.h"
#include "gmc/gmc_7_1_sh_mask.h"
#include "cik_structs.h"

#define CIK_PIPE_PER_MEC	(4)

#define AMDKFD_SKIP_UNCOMPILED_CODE 1

enum {
	MAX_TRAPID = 8,		/* 3 bits in the bitfield. */
	MAX_WATCH_ADDRESSES = 4
};

enum {
	ADDRESS_WATCH_REG_ADDR_HI = 0,
	ADDRESS_WATCH_REG_ADDR_LO,
	ADDRESS_WATCH_REG_CNTL,
	ADDRESS_WATCH_REG_MAX
};

/*  not defined in the CI/KV reg file  */
enum {
	ADDRESS_WATCH_REG_CNTL_ATC_BIT = 0x10000000UL,
	ADDRESS_WATCH_REG_CNTL_DEFAULT_MASK = 0x00FFFFFF,
	ADDRESS_WATCH_REG_ADDLOW_MASK_EXTENTION = 0x03000000,
	/* extend the mask to 26 bits in order to match the low address field. */
	ADDRESS_WATCH_REG_ADDLOW_SHIFT = 6,
	ADDRESS_WATCH_REG_ADDHIGH_MASK = 0xFFFF
};

static const uint32_t watchRegs[MAX_WATCH_ADDRESSES * ADDRESS_WATCH_REG_MAX] = {
	mmTCP_WATCH0_ADDR_H, mmTCP_WATCH0_ADDR_L, mmTCP_WATCH0_CNTL,
	mmTCP_WATCH1_ADDR_H, mmTCP_WATCH1_ADDR_L, mmTCP_WATCH1_CNTL,
	mmTCP_WATCH2_ADDR_H, mmTCP_WATCH2_ADDR_L, mmTCP_WATCH2_CNTL,
	mmTCP_WATCH3_ADDR_H, mmTCP_WATCH3_ADDR_L, mmTCP_WATCH3_CNTL
};

union TCP_WATCH_CNTL_BITS {
	struct {
		uint32_t mask:24;
		uint32_t vmid:4;
		uint32_t atc:1;
		uint32_t mode:2;
		uint32_t valid:1;
	} bitfields, bits;
	uint32_t u32All;
	signed int i32All;
	float f32All;
};

static int create_process_vm(struct kgd_dev *kgd, void **vm);
static void destroy_process_vm(struct kgd_dev *kgd, void *vm);

static uint32_t get_process_page_dir(void *vm);

static int open_graphic_handle(struct kgd_dev *kgd, uint64_t va, void *vm, int fd, uint32_t handle, struct kgd_mem **mem);
static int map_memory_to_gpu(struct kgd_dev *kgd, struct kgd_mem *mem);
static int unmap_memory_from_gpu(struct kgd_dev *kgd, struct kgd_mem *mem);
static int alloc_memory_of_gpu(struct kgd_dev *kgd, uint64_t va,
			size_t size, void *vm, struct kgd_mem **mem);
static int free_memory_of_gpu(struct kgd_dev *kgd, struct kgd_mem *mem);

static uint16_t get_fw_version(struct kgd_dev *kgd, enum kgd_engine_type type);

/*
 * Register access functions
 */

static void kgd_program_sh_mem_settings(struct kgd_dev *kgd, uint32_t vmid, uint32_t sh_mem_config,
		uint32_t sh_mem_ape1_base, uint32_t sh_mem_ape1_limit, uint32_t sh_mem_bases);
static int kgd_set_pasid_vmid_mapping(struct kgd_dev *kgd, unsigned int pasid, unsigned int vmid);
static int kgd_init_pipeline(struct kgd_dev *kgd, uint32_t pipe_id, uint32_t hpd_size, uint64_t hpd_gpu_addr);
static int kgd_init_interrupts(struct kgd_dev *kgd, uint32_t pipe_id);
static int kgd_hqd_load(struct kgd_dev *kgd, void *mqd, uint32_t pipe_id, uint32_t queue_id, uint32_t __user *wptr);
static int kgd_hqd_sdma_load(struct kgd_dev *kgd, void *mqd);
static bool kgd_hqd_is_occupied(struct kgd_dev *kgd, uint64_t queue_address,
		uint32_t pipe_id, uint32_t queue_id);
static bool kgd_hqd_sdma_is_occupied(struct kgd_dev *kgd, void *mqd);
static int kgd_hqd_destroy(struct kgd_dev *kgd, uint32_t reset_type,
				unsigned int timeout, uint32_t pipe_id,
				uint32_t queue_id);
static int kgd_hqd_sdma_destroy(struct kgd_dev *kgd, void *mqd,
				unsigned int timeout);
static int kgd_address_watch_disable(struct kgd_dev *kgd);
static int kgd_address_watch_execute(struct kgd_dev *kgd,
					unsigned int watch_point_id,
					uint32_t cntl_val,
					uint32_t addr_hi,
					uint32_t addr_lo);
static int kgd_wave_control_execute(struct kgd_dev *kgd,
					uint32_t gfx_index_val,
					uint32_t sq_cmd);
static uint32_t kgd_address_watch_get_offset(struct kgd_dev *kgd,
					unsigned int watch_point_id,
					unsigned int reg_offset);

static bool get_atc_vmid_pasid_mapping_valid(struct kgd_dev *kgd, uint8_t vmid);
static uint16_t get_atc_vmid_pasid_mapping_pasid(struct kgd_dev *kgd,
							uint8_t vmid);
static void write_vmid_invalidate_request(struct kgd_dev *kgd, uint8_t vmid);
static void set_num_of_requests(struct kgd_dev *dev, uint8_t num_of_req);
static int alloc_memory_of_scratch(struct kgd_dev *kgd,
					 uint64_t va, uint32_t vmid);
static int write_config_static_mem(struct kgd_dev *kgd, bool swizzle_enable,
		uint8_t element_size, uint8_t index_stride, uint8_t mtype);

static const struct kfd2kgd_calls kfd2kgd = {
	.init_gtt_mem_allocation = alloc_gtt_mem,
	.free_gtt_mem = free_gtt_mem,
	.get_vmem_size = get_vmem_size,
	.get_gpu_clock_counter = get_gpu_clock_counter,
	.get_max_engine_clock_in_mhz = get_max_engine_clock_in_mhz,
	.create_process_vm = create_process_vm,
	.destroy_process_vm = destroy_process_vm,
	.get_process_page_dir = get_process_page_dir,
	.open_graphic_handle = open_graphic_handle,
	.program_sh_mem_settings = kgd_program_sh_mem_settings,
	.set_pasid_vmid_mapping = kgd_set_pasid_vmid_mapping,
	.init_pipeline = kgd_init_pipeline,
	.init_interrupts = kgd_init_interrupts,
	.hqd_load = kgd_hqd_load,
	.hqd_sdma_load = kgd_hqd_sdma_load,
	.hqd_is_occupied = kgd_hqd_is_occupied,
	.hqd_sdma_is_occupied = kgd_hqd_sdma_is_occupied,
	.hqd_destroy = kgd_hqd_destroy,
	.hqd_sdma_destroy = kgd_hqd_sdma_destroy,
	.address_watch_disable = kgd_address_watch_disable,
	.address_watch_execute = kgd_address_watch_execute,
	.wave_control_execute = kgd_wave_control_execute,
	.address_watch_get_offset = kgd_address_watch_get_offset,
	.get_atc_vmid_pasid_mapping_pasid = get_atc_vmid_pasid_mapping_pasid,
	.get_atc_vmid_pasid_mapping_valid = get_atc_vmid_pasid_mapping_valid,
	.write_vmid_invalidate_request = write_vmid_invalidate_request,
	.alloc_memory_of_gpu = alloc_memory_of_gpu,
	.free_memory_of_gpu = free_memory_of_gpu,
	.map_memory_to_gpu = map_memory_to_gpu,
	.unmap_memory_to_gpu = unmap_memory_from_gpu,
	.get_fw_version = get_fw_version,
	.set_num_of_requests = set_num_of_requests,
	.get_cu_info = get_cu_info,
	.alloc_memory_of_scratch = alloc_memory_of_scratch,
	.write_config_static_mem = write_config_static_mem
};

struct kfd2kgd_calls *amdgpu_amdkfd_gfx_7_get_functions()
{
	return (struct kfd2kgd_calls *)&kfd2kgd;
}

/*
 * Creates a VM context for HSA process
 */
static int create_process_vm(struct kgd_dev *kgd, void **vm)
{
	int ret;
	struct amdgpu_vm *new_vm;
	struct amdgpu_device *adev = (struct amdgpu_device *) kgd;

	BUG_ON(kgd == NULL);
	BUG_ON(vm == NULL);

	new_vm = kzalloc(sizeof(struct amdgpu_vm), GFP_KERNEL);
	if (new_vm == NULL)
		return -ENOMEM;

	/* Initialize the VM context, allocate the page directory and zero it */
	ret = amdgpu_vm_init(adev, new_vm);
	if (ret != 0) {
		/* Undo everything related to the new VM context */
		amdgpu_vm_fini(adev, new_vm);
		kfree(new_vm);
		new_vm = NULL;
	}

	/* Pin the PD directory*/
	amdgpu_bo_reserve(new_vm->page_directory, true);
	amdgpu_bo_pin(new_vm->page_directory, AMDGPU_GEM_DOMAIN_VRAM, NULL);
	amdgpu_bo_unreserve(new_vm->page_directory);
#if 0
	new_vm->pd_gpu_addr = amdgpu_bo_gpu_offset(new_vm->page_directory);
#endif
	*vm = (void *) new_vm;

	return ret;
}

/*
 * Destroys a VM context of HSA process
 */
static void destroy_process_vm(struct kgd_dev *kgd, void *vm)
{
	struct amdgpu_device *adev = (struct amdgpu_device *) kgd;
	struct amdgpu_vm *rvm = (struct amdgpu_vm *) vm;

	BUG_ON(kgd == NULL);
	BUG_ON(vm == NULL);

	/* Unpin the PD directory*/
	amdgpu_bo_reserve(rvm->page_directory, true);
	amdgpu_bo_unpin(rvm->page_directory);
	amdgpu_bo_unreserve(rvm->page_directory);

	/* Release the VM context */
	amdgpu_vm_fini(adev, rvm);
	kfree(vm);
}

static uint32_t get_process_page_dir(void *vm)
{
#if 0
	struct amdgpu_vm *rvm = (struct amdgpu_vm *) vm;

	BUG_ON(vm == NULL);

	return rvm->pd_gpu_addr >> AMDGPU_GPU_PAGE_SHIFT;
#endif
	return 0;
}

static int open_graphic_handle(struct kgd_dev *kgd, uint64_t va, void *vm,
				int fd, uint32_t handle, struct kgd_mem **mem)
{
	return 0;
}

static inline struct amdgpu_device *get_amdgpu_device(struct kgd_dev *kgd)
{
	return (struct amdgpu_device *)kgd;
}

static void lock_srbm(struct kgd_dev *kgd, uint32_t mec, uint32_t pipe,
			uint32_t queue, uint32_t vmid)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	uint32_t value = PIPEID(pipe) | MEID(mec) | VMID(vmid) | QUEUEID(queue);

	mutex_lock(&adev->srbm_mutex);
	WREG32(mmSRBM_GFX_CNTL, value);
}

static void unlock_srbm(struct kgd_dev *kgd)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);

	WREG32(mmSRBM_GFX_CNTL, 0);
	mutex_unlock(&adev->srbm_mutex);
}

static void acquire_queue(struct kgd_dev *kgd, uint32_t pipe_id,
				uint32_t queue_id)
{
	uint32_t mec = (++pipe_id / CIK_PIPE_PER_MEC) + 1;
	uint32_t pipe = (pipe_id % CIK_PIPE_PER_MEC);

	lock_srbm(kgd, mec, pipe, queue_id, 0);
}

static void release_queue(struct kgd_dev *kgd)
{
	unlock_srbm(kgd);
}

static void kgd_program_sh_mem_settings(struct kgd_dev *kgd, uint32_t vmid,
					uint32_t sh_mem_config,
					uint32_t sh_mem_ape1_base,
					uint32_t sh_mem_ape1_limit,
					uint32_t sh_mem_bases)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);

	lock_srbm(kgd, 0, 0, 0, vmid);

	WREG32(mmSH_MEM_CONFIG, sh_mem_config);
	WREG32(mmSH_MEM_APE1_BASE, sh_mem_ape1_base);
	WREG32(mmSH_MEM_APE1_LIMIT, sh_mem_ape1_limit);
	WREG32(mmSH_MEM_BASES, sh_mem_bases);

	unlock_srbm(kgd);
}

static int kgd_set_pasid_vmid_mapping(struct kgd_dev *kgd, unsigned int pasid,
					unsigned int vmid)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);

	/*
	 * We have to assume that there is no outstanding mapping.
	 * The ATC_VMID_PASID_MAPPING_UPDATE_STATUS bit could be 0 because a mapping
	 * is in progress or because a mapping finished and the SW cleared it.
	 * So the protocol is to always wait & clear.
	 */
	uint32_t pasid_mapping = (pasid == 0) ? 0 : (uint32_t)pasid | ATC_VMID0_PASID_MAPPING__VALID_MASK;

	WREG32(mmATC_VMID0_PASID_MAPPING + vmid, pasid_mapping);

	while (!(RREG32(mmATC_VMID_PASID_MAPPING_UPDATE_STATUS) & (1U << vmid)))
		cpu_relax();
	WREG32(mmATC_VMID_PASID_MAPPING_UPDATE_STATUS, 1U << vmid);

	/* Mapping vmid to pasid also for IH block */
	WREG32(mmIH_VMID_0_LUT + vmid, pasid_mapping);

	return 0;
}

static int kgd_init_pipeline(struct kgd_dev *kgd, uint32_t pipe_id,
				uint32_t hpd_size, uint64_t hpd_gpu_addr)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);

	uint32_t mec = (++pipe_id / CIK_PIPE_PER_MEC) + 1;
	uint32_t pipe = (pipe_id % CIK_PIPE_PER_MEC);

	lock_srbm(kgd, mec, pipe, 0, 0);
	WREG32(mmCP_HPD_EOP_BASE_ADDR, lower_32_bits(hpd_gpu_addr >> 8));
	WREG32(mmCP_HPD_EOP_BASE_ADDR_HI, upper_32_bits(hpd_gpu_addr >> 8));
	WREG32(mmCP_HPD_EOP_VMID, 0);
	WREG32(mmCP_HPD_EOP_CONTROL, hpd_size);
	unlock_srbm(kgd);

	return 0;
}

static int kgd_init_interrupts(struct kgd_dev *kgd, uint32_t pipe_id)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	uint32_t mec;
	uint32_t pipe;

	mec = (++pipe_id / CIK_PIPE_PER_MEC) + 1;
	pipe = (pipe_id % CIK_PIPE_PER_MEC);

	lock_srbm(kgd, mec, pipe, 0, 0);

	WREG32(mmCPC_INT_CNTL, CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK |
			CP_INT_CNTL_RING0__OPCODE_ERROR_INT_ENABLE_MASK);

	unlock_srbm(kgd);

	return 0;
}

static inline uint32_t get_sdma_base_addr(struct cik_sdma_rlc_registers *m)
{
	uint32_t retval;

	retval = m->sdma_engine_id * SDMA1_REGISTER_OFFSET +
			m->sdma_queue_id * KFD_CIK_SDMA_QUEUE_OFFSET;
	pr_err("kfd: sdma base address: 0x%x\n", retval);

	return retval;
}

static inline struct cik_mqd *get_mqd(void *mqd)
{
	return (struct cik_mqd *)mqd;
}

static inline struct cik_sdma_rlc_registers *get_sdma_mqd(void *mqd)
{
	return (struct cik_sdma_rlc_registers *)mqd;
}

static int kgd_hqd_load(struct kgd_dev *kgd, void *mqd, uint32_t pipe_id,
			uint32_t queue_id, uint32_t __user *wptr)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	uint32_t wptr_shadow, is_wptr_shadow_valid;
	struct cik_mqd *m;

	m = get_mqd(mqd);

	is_wptr_shadow_valid = !get_user(wptr_shadow, wptr);

	acquire_queue(kgd, pipe_id, queue_id);

	WREG32(mmCOMPUTE_STATIC_THREAD_MGMT_SE0,
		m->compute_static_thread_mgmt_se0);
	WREG32(mmCOMPUTE_STATIC_THREAD_MGMT_SE1,
		m->compute_static_thread_mgmt_se1);
	WREG32(mmCOMPUTE_STATIC_THREAD_MGMT_SE2,
		m->compute_static_thread_mgmt_se2);
	WREG32(mmCOMPUTE_STATIC_THREAD_MGMT_SE3,
		m->compute_static_thread_mgmt_se3);

	WREG32(mmCP_MQD_BASE_ADDR, m->cp_mqd_base_addr_lo);
	WREG32(mmCP_MQD_BASE_ADDR_HI, m->cp_mqd_base_addr_hi);
	WREG32(mmCP_MQD_CONTROL, m->cp_mqd_control);

	WREG32(mmCP_HQD_PQ_BASE, m->cp_hqd_pq_base_lo);
	WREG32(mmCP_HQD_PQ_BASE_HI, m->cp_hqd_pq_base_hi);
	WREG32(mmCP_HQD_PQ_CONTROL, m->cp_hqd_pq_control);

	WREG32(mmCP_HQD_IB_CONTROL, m->cp_hqd_ib_control);
	WREG32(mmCP_HQD_IB_BASE_ADDR, m->cp_hqd_ib_base_addr_lo);
	WREG32(mmCP_HQD_IB_BASE_ADDR_HI, m->cp_hqd_ib_base_addr_hi);

	WREG32(mmCP_HQD_IB_RPTR, m->cp_hqd_ib_rptr);

	WREG32(mmCP_HQD_PERSISTENT_STATE, m->cp_hqd_persistent_state);
	WREG32(mmCP_HQD_SEMA_CMD, m->cp_hqd_sema_cmd);
	WREG32(mmCP_HQD_MSG_TYPE, m->cp_hqd_msg_type);

	WREG32(mmCP_HQD_ATOMIC0_PREOP_LO, m->cp_hqd_atomic0_preop_lo);
	WREG32(mmCP_HQD_ATOMIC0_PREOP_HI, m->cp_hqd_atomic0_preop_hi);
	WREG32(mmCP_HQD_ATOMIC1_PREOP_LO, m->cp_hqd_atomic1_preop_lo);
	WREG32(mmCP_HQD_ATOMIC1_PREOP_HI, m->cp_hqd_atomic1_preop_hi);

	WREG32(mmCP_HQD_PQ_RPTR_REPORT_ADDR, m->cp_hqd_pq_rptr_report_addr_lo);
	WREG32(mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI, m->cp_hqd_pq_rptr_report_addr_hi);
	WREG32(mmCP_HQD_PQ_RPTR, m->cp_hqd_pq_rptr);

	WREG32(mmCP_HQD_PQ_WPTR_POLL_ADDR, m->cp_hqd_pq_wptr_poll_addr_lo);
	WREG32(mmCP_HQD_PQ_WPTR_POLL_ADDR_HI, m->cp_hqd_pq_wptr_poll_addr_hi);

	WREG32(mmCP_HQD_PQ_DOORBELL_CONTROL, m->cp_hqd_pq_doorbell_control);

	WREG32(mmCP_HQD_VMID, m->cp_hqd_vmid);

	WREG32(mmCP_HQD_QUANTUM, m->cp_hqd_quantum);

	WREG32(mmCP_HQD_PIPE_PRIORITY, m->cp_hqd_pipe_priority);
	WREG32(mmCP_HQD_QUEUE_PRIORITY, m->cp_hqd_queue_priority);

	WREG32(mmCP_HQD_IQ_RPTR, m->cp_hqd_iq_rptr);

	if (is_wptr_shadow_valid)
		WREG32(mmCP_HQD_PQ_WPTR, wptr_shadow);

	WREG32(mmCP_HQD_ACTIVE, m->cp_hqd_active);
	release_queue(kgd);

	return 0;
}

static int kgd_hqd_sdma_load(struct kgd_dev *kgd, void *mqd)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	struct cik_sdma_rlc_registers *m;
	uint32_t sdma_base_addr;

	m = get_sdma_mqd(mqd);
	sdma_base_addr = get_sdma_base_addr(m);

	WREG32(sdma_base_addr + mmSDMA0_RLC0_VIRTUAL_ADDR, m->sdma_rlc_virtual_addr);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_BASE, m->sdma_rlc_rb_base);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_BASE_HI, m->sdma_rlc_rb_base_hi);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_RPTR_ADDR_LO, m->sdma_rlc_rb_rptr_addr_lo);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_RPTR_ADDR_HI, m->sdma_rlc_rb_rptr_addr_hi);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_DOORBELL, m->sdma_rlc_doorbell);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL, m->sdma_rlc_rb_cntl);

	return 0;
}

static bool kgd_hqd_is_occupied(struct kgd_dev *kgd, uint64_t queue_address,
				uint32_t pipe_id, uint32_t queue_id)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	uint32_t act;
	bool retval = false;
	uint32_t low, high;

	acquire_queue(kgd, pipe_id, queue_id);
	act = RREG32(mmCP_HQD_ACTIVE);
	if (act) {
		low = lower_32_bits(queue_address >> 8);
		high = upper_32_bits(queue_address >> 8);

		if (low == RREG32(mmCP_HQD_PQ_BASE) &&
				high == RREG32(mmCP_HQD_PQ_BASE_HI))
			retval = true;
	}
	release_queue(kgd);
	return retval;
}

static bool kgd_hqd_sdma_is_occupied(struct kgd_dev *kgd, void *mqd)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	struct cik_sdma_rlc_registers *m;
	uint32_t sdma_base_addr;
	uint32_t sdma_rlc_rb_cntl;

	m = get_sdma_mqd(mqd);
	sdma_base_addr = get_sdma_base_addr(m);

	sdma_rlc_rb_cntl = RREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL);

	if (sdma_rlc_rb_cntl & SDMA0_RLC0_RB_CNTL__RB_ENABLE_MASK)
		return true;

	return false;
}

static int kgd_hqd_destroy(struct kgd_dev *kgd, uint32_t reset_type,
				unsigned int timeout, uint32_t pipe_id,
				uint32_t queue_id)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	uint32_t temp;

	acquire_queue(kgd, pipe_id, queue_id);
	WREG32(mmCP_HQD_PQ_DOORBELL_CONTROL, 0);

	WREG32(mmCP_HQD_DEQUEUE_REQUEST, reset_type);

	while (true) {
		temp = RREG32(mmCP_HQD_ACTIVE);
		if (temp & CP_HQD_ACTIVE__ACTIVE__SHIFT)
			break;
		if (timeout == 0) {
			pr_err("kfd: cp queue preemption time out (%dms)\n",
				temp);
			return -ETIME;
		}
		msleep(20);
		timeout -= 20;
	}

	release_queue(kgd);
	return 0;
}

static int kgd_hqd_sdma_destroy(struct kgd_dev *kgd, void *mqd,
				unsigned int timeout)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	struct cik_sdma_rlc_registers *m;
	uint32_t sdma_base_addr;
	uint32_t temp;

	m = get_sdma_mqd(mqd);
	sdma_base_addr = get_sdma_base_addr(m);

	temp = RREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL);
	temp = temp & ~SDMA0_RLC0_RB_CNTL__RB_ENABLE_MASK;
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL, temp);

	while (true) {
		temp = RREG32(sdma_base_addr + mmSDMA0_RLC0_CONTEXT_STATUS);
		if (temp & SDMA0_STATUS_REG__RB_CMD_IDLE__SHIFT)
			break;
		if (timeout == 0)
			return -ETIME;
		msleep(20);
		timeout -= 20;
	}

	WREG32(sdma_base_addr + mmSDMA0_RLC0_DOORBELL, 0);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_RPTR, 0);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_WPTR, 0);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_BASE, 0);

	return 0;
}

static int kgd_address_watch_disable(struct kgd_dev *kgd)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	union TCP_WATCH_CNTL_BITS cntl;
	unsigned int i;

	cntl.u32All = 0;

	cntl.bitfields.valid = 0;
	cntl.bitfields.mask = ADDRESS_WATCH_REG_CNTL_DEFAULT_MASK;
	cntl.bitfields.atc = 1;

	/* Turning off this address until we set all the registers */
	for (i = 0; i < MAX_WATCH_ADDRESSES; i++)
		WREG32(watchRegs[i * ADDRESS_WATCH_REG_MAX + ADDRESS_WATCH_REG_CNTL],
			cntl.u32All);

	return 0;
}

static int kgd_address_watch_execute(struct kgd_dev *kgd,
					unsigned int watch_point_id,
					uint32_t cntl_val,
					uint32_t addr_hi,
					uint32_t addr_lo)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	union TCP_WATCH_CNTL_BITS cntl;

	cntl.u32All = cntl_val;

	/* Turning off this watch point until we set all the registers */
	cntl.bitfields.valid = 0;
	WREG32(watchRegs[watch_point_id * ADDRESS_WATCH_REG_MAX + ADDRESS_WATCH_REG_CNTL],
		cntl.u32All);

	WREG32(watchRegs[watch_point_id * ADDRESS_WATCH_REG_MAX + ADDRESS_WATCH_REG_ADDR_HI],
		addr_hi);

	WREG32(watchRegs[watch_point_id * ADDRESS_WATCH_REG_MAX + ADDRESS_WATCH_REG_ADDR_LO],
		addr_lo);

	/* Enable the watch point */
	cntl.bitfields.valid = 1;

	WREG32(watchRegs[watch_point_id * ADDRESS_WATCH_REG_MAX + ADDRESS_WATCH_REG_CNTL],
		cntl.u32All);

	return 0;
}

static int kgd_wave_control_execute(struct kgd_dev *kgd,
					uint32_t gfx_index_val,
					uint32_t sq_cmd)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	uint32_t data;

	mutex_lock(&adev->grbm_idx_mutex);

	WREG32(mmGRBM_GFX_INDEX, gfx_index_val);
	WREG32(mmSQ_CMD, sq_cmd);

	/*  Restore the GRBM_GFX_INDEX register  */

	data = GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK |
		GRBM_GFX_INDEX__SH_BROADCAST_WRITES_MASK |
		GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK;

	WREG32(mmGRBM_GFX_INDEX, data);

	mutex_unlock(&adev->grbm_idx_mutex);

	return 0;
}

static uint32_t kgd_address_watch_get_offset(struct kgd_dev *kgd,
					unsigned int watch_point_id,
					unsigned int reg_offset)
{
	return watchRegs[watch_point_id * ADDRESS_WATCH_REG_MAX + reg_offset];
}

static bool get_atc_vmid_pasid_mapping_valid(struct kgd_dev *kgd,
							uint8_t vmid)
{
	uint32_t reg;
	struct amdgpu_device *adev = (struct amdgpu_device *) kgd;

	reg = RREG32(mmATC_VMID0_PASID_MAPPING + vmid);
	return reg & ATC_VMID0_PASID_MAPPING__VALID_MASK;
}

static uint16_t get_atc_vmid_pasid_mapping_pasid(struct kgd_dev *kgd,
								uint8_t vmid)
{
	uint32_t reg;
	struct amdgpu_device *adev = (struct amdgpu_device *) kgd;

	reg = RREG32(mmATC_VMID0_PASID_MAPPING + vmid);
	return reg & ATC_VMID0_PASID_MAPPING__VALID_MASK;
}

static void write_vmid_invalidate_request(struct kgd_dev *kgd, uint8_t vmid)
{
	struct amdgpu_device *adev = (struct amdgpu_device *) kgd;

	WREG32(mmVM_INVALIDATE_REQUEST, 1 << vmid);
}

static int write_config_static_mem(struct kgd_dev *kgd, bool swizzle_enable,
		uint8_t element_size, uint8_t index_stride, uint8_t mtype)
{
	uint32_t reg;
	struct amdgpu_device *adev = (struct amdgpu_device *) kgd;

	reg = swizzle_enable << SH_STATIC_MEM_CONFIG__SWIZZLE_ENABLE__SHIFT |
		element_size << SH_STATIC_MEM_CONFIG__ELEMENT_SIZE__SHIFT |
		index_stride << SH_STATIC_MEM_CONFIG__INDEX_STRIDE__SHIFT |
		mtype << SH_STATIC_MEM_CONFIG__PRIVATE_MTYPE__SHIFT;

	WREG32(mmSH_STATIC_MEM_CONFIG, reg);
	return 0;
}
static int alloc_memory_of_scratch(struct kgd_dev *kgd,
				 uint64_t va, uint32_t vmid)
{
	struct amdgpu_device *adev = (struct amdgpu_device *) kgd;

	lock_srbm(kgd, 0, 0, 0, vmid);
	WREG32(mmSH_HIDDEN_PRIVATE_BASE_VMID, va);
	unlock_srbm(kgd);

	return 0;
}


static int alloc_memory_of_gpu(struct kgd_dev *kgd, uint64_t va, size_t size,
				void *vm, struct kgd_mem **mem)
{
	return -EFAULT;
}

static int free_memory_of_gpu(struct kgd_dev *kgd, struct kgd_mem *mem)
{
	return -EFAULT;
}

static int map_memory_to_gpu(struct kgd_dev *kgd, struct kgd_mem *mem)
{
	return -EFAULT;
}

static int unmap_memory_from_gpu(struct kgd_dev *kgd, struct kgd_mem *mem)
{
	return -EFAULT;
}

static uint16_t get_fw_version(struct kgd_dev *kgd, enum kgd_engine_type type)
{
	struct amdgpu_device *adev = (struct amdgpu_device *) kgd;
	const union amdgpu_firmware_header *hdr;

	BUG_ON(kgd == NULL);

	switch (type) {
	case KGD_ENGINE_PFP:
		hdr = (const union amdgpu_firmware_header *)
							adev->gfx.pfp_fw->data;
		break;

	case KGD_ENGINE_ME:
		hdr = (const union amdgpu_firmware_header *)
							adev->gfx.me_fw->data;
		break;

	case KGD_ENGINE_CE:
		hdr = (const union amdgpu_firmware_header *)
							adev->gfx.ce_fw->data;
		break;

	case KGD_ENGINE_MEC1:
		hdr = (const union amdgpu_firmware_header *)
							adev->gfx.mec_fw->data;
		break;

	case KGD_ENGINE_MEC2:
		hdr = (const union amdgpu_firmware_header *)
							adev->gfx.mec2_fw->data;
		break;

	case KGD_ENGINE_RLC:
		hdr = (const union amdgpu_firmware_header *)
							adev->gfx.rlc_fw->data;
		break;

	case KGD_ENGINE_SDMA1:
		hdr = (const union amdgpu_firmware_header *)
							adev->sdma[0].fw->data;
		break;

	case KGD_ENGINE_SDMA2:
		hdr = (const union amdgpu_firmware_header *)
							adev->sdma[1].fw->data;
		break;

	default:
		return 0;
	}

	if (hdr == NULL)
		return 0;

	/* Only 12 bit in use*/
	return hdr->common.ucode_version;
}

static void set_num_of_requests(struct kgd_dev *dev, uint8_t num_of_req)
{
	uint32_t value;
	struct amdgpu_device *adev = get_amdgpu_device(dev);

	value = RREG32(mmATC_ATS_DEBUG);
	value &= ~ATC_ATS_DEBUG__NUM_REQUESTS_AT_ERR_MASK;
	value |= (num_of_req << ATC_ATS_DEBUG__NUM_REQUESTS_AT_ERR__SHIFT);

	WREG32(mmATC_ATS_DEBUG, value);
}
