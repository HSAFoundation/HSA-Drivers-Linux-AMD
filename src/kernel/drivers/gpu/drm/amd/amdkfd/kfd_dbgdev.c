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
 *
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/device.h>

#include "kfd_pm4_headers.h"
#include "kfd_pm4_headers_diq.h"
#include "kfd_kernel_queue.h"
#include "kfd_priv.h"
#include "kfd_pm4_opcodes.h"
#include "cik_regs.h"
#include "kfd_dbgmgr.h"
#include "kfd_dbgdev.h"
#include "kfd_device_queue_manager.h"
#include "../../radeon/cik_reg.h"

static void dbgdev_address_watch_disable_nodiq(struct kfd_dev *dev)
{
	dev->kfd2kgd->address_watch_disable(dev->kgd);
}

static int dbgdev_diq_submit_ib(struct kfd_dbgdev *dbgdev,
				unsigned int pasid, uint64_t vmid0_address,
				uint32_t *packet_buff, size_t size_in_bytes)
{
	int status = 0;
	unsigned int *ib_packet_buff = NULL;
	struct pm4__release_mem *rm_packet;
	struct pm4__indirect_buffer_pasid *ib_packet;
	struct kernel_queue *kq = dbgdev->kq;
	size_t pq_packets_size_in_bytes = sizeof(struct pm4__release_mem) + sizeof(struct pm4__indirect_buffer_pasid);
	struct kfd_mem_obj *mem_obj;

	uint64_t *rm_state = NULL;

	union ULARGE_INTEGER *largep;
	union ULARGE_INTEGER addr;

	do {
		if ((kq == NULL) || (packet_buff == NULL) || (size_in_bytes == 0)) {
			pr_debug("Error! kfd: In func %s >> Illegal packet parameters\n", __func__);
			status = -EINVAL;
			break;
		}
		/* todo - enter proper locking to be multithreaded safe */

		/* We acquire a buffer from DIQ
		 * The receive packet buff will be sitting on the Indirect Buffer
		 * and in the PQ we put the IB packet + sync packet(s).
		 */
		status = kq->ops.acquire_packet_buffer(kq, pq_packets_size_in_bytes / sizeof(uint32_t), &ib_packet_buff);
		if (status != 0) {
			pr_debug("Error! kfd: In func %s >> acquire_packet_buffer failed\n", __func__);
			break;
		}

		memset(ib_packet_buff, 0, pq_packets_size_in_bytes);

		ib_packet = (struct pm4__indirect_buffer_pasid *) (ib_packet_buff);

		ib_packet->header.count = 3;
		ib_packet->header.opcode = IT_INDIRECT_BUFFER_PASID;
		ib_packet->header.type = PM4_TYPE_3;

		largep = (union ULARGE_INTEGER *) &vmid0_address;

		ib_packet->bitfields2.ib_base_lo = largep->u.low_part >> 2;
		ib_packet->bitfields3.ib_base_hi = largep->u.high_part;

		ib_packet->control = (1 << 23) | (1 << 31) |
				((size_in_bytes / sizeof(uint32_t)) & 0xfffff);

		ib_packet->bitfields5.pasid = pasid;

		/*
		 * for now we use release mem for GPU-CPU synchronization
		 * Consider WaitRegMem + WriteData as a better alternative
		 * we get a GART allocations ( gpu/cpu mapping),
		 * for the sync variable, and wait until:
		 * (a) Sync with HW
		 * (b) Sync var is written by CP to mem.
		 */
		rm_packet = (struct pm4__release_mem *) (ib_packet_buff +
				(sizeof(struct pm4__indirect_buffer_pasid) / sizeof(unsigned int)));

		status = kfd_gtt_sa_allocate(dbgdev->dev, sizeof(uint64_t),
						&mem_obj);

		if (status == 0) {

			rm_state = (uint64_t *) mem_obj->cpu_ptr;

			*rm_state = QUEUESTATE__ACTIVE_COMPLETION_PENDING;

			rm_packet->header.opcode = IT_RELEASE_MEM;
			rm_packet->header.type = PM4_TYPE_3;
			rm_packet->header.count = sizeof(struct pm4__release_mem) / sizeof(unsigned int) - 2;

			rm_packet->bitfields2.event_type = CACHE_FLUSH_AND_INV_TS_EVENT;
			rm_packet->bitfields2.event_index = event_index___release_mem__end_of_pipe;
			rm_packet->bitfields2.cache_policy = cache_policy___release_mem__lru;
			rm_packet->bitfields2.atc = 0;
			rm_packet->bitfields2.tc_wb_action_ena = 1;

			addr.quad_part = mem_obj->gpu_addr;

			rm_packet->bitfields4.address_lo_32b = addr.u.low_part >> 2;
			rm_packet->address_hi = addr.u.high_part;

			rm_packet->bitfields3.data_sel = data_sel___release_mem__send_64_bit_data;
			rm_packet->bitfields3.int_sel = int_sel___release_mem__send_data_after_write_confirm;
			rm_packet->bitfields3.dst_sel = dst_sel___release_mem__memory_controller;

			rm_packet->data_lo = QUEUESTATE__ACTIVE;

			kq->ops.submit_packet(kq);

			/* Wait till CP writes sync code: */

			status = amdkfd_fence_wait_timeout(
					(unsigned int *) rm_state,
					QUEUESTATE__ACTIVE, 1500);

		} else {
			pr_debug("Error! kfd: In func %s >> failed to allocate GART memory\n", __func__);
		}
	} while (false);

	if (rm_state != NULL)
		kfd_gtt_sa_free(dbgdev->dev, mem_obj);

	return status;
}

static int dbgdev_register_nodiq(struct kfd_dbgdev *dbgdev)
{
	/* no action is needed in this case, just make sure diq will not be used */

	dbgdev->kq = NULL;

	return 0;
}

static int dbgdev_register_diq(struct kfd_dbgdev *dbgdev)
{

	int status = 0;
	struct kernel_queue *kq = NULL;
	struct queue_properties properties;
	unsigned int qid;
	struct process_queue_manager *pqm = dbgdev->pqm;

	do {

		if (!pqm) {
			pr_debug("Error! kfd: In func %s >> No PQM\n", __func__);
			status = -EFAULT;
			break;
		}

		status = pqm_create_queue(dbgdev->pqm, dbgdev->dev, NULL, &properties, 0, KFD_QUEUE_TYPE_DIQ, &qid);

		if (status != 0) {
			pr_debug("Error! kfd: In func %s >> Create Queue failed\n", __func__);
			break;
		}

		pr_debug("kfd: DIQ Created with queue id: %d\n", qid);

		kq = pqm_get_kernel_queue(dbgdev->pqm, qid);

		if (kq == NULL) {
			pr_debug("Error! kfd: In func %s >> Error getting Kernel Queue\n", __func__);
			status = -ENOMEM;
			break;
		}

		dbgdev->kq = kq;

	} while (false);

	return status;
}

static int dbgdev_unregister_nodiq(struct kfd_dbgdev *dbgdev)
{
	/* disable watch address */

	dbgdev_address_watch_disable_nodiq(dbgdev->dev);
	return 0;
}

static int dbgdev_unregister_diq(struct kfd_dbgdev *dbgdev)
{
	/* todo - if needed, kill wavefronts and disable watch */
	int status = 0;

	if (dbgdev->pqm) {

		pqm_destroy_queue(dbgdev->pqm, dbgdev->kq->queue->properties.queue_id);
		dbgdev->kq = NULL;
	} else {
		pr_debug("Error! kfd: In func %s >> destroy queue failed\n", __func__);
		status = -EFAULT;
	}

	return status;
}

static void dbgdev_address_watch_set_registers(
			const struct dbg_address_watch_info *adw_info,
			union TCP_WATCH_ADDR_H_BITS *addrHi,
			union TCP_WATCH_ADDR_L_BITS *addrLo,
			union TCP_WATCH_CNTL_BITS *cntl,
			unsigned int index, unsigned int vmid)
{
	union ULARGE_INTEGER addr;

	addr.quad_part = 0;
	addrHi->u32All = 0;
	addrLo->u32All = 0;
	cntl->u32All = 0;

	if (adw_info->watch_mask != NULL)
		cntl->bitfields.mask = (uint32_t) (adw_info->watch_mask[index] & ADDRESS_WATCH_REG_CNTL_DEFAULT_MASK);
	else
		cntl->bitfields.mask = ADDRESS_WATCH_REG_CNTL_DEFAULT_MASK;

	addr.quad_part = (unsigned long long) adw_info->watch_address[index];

	addrHi->bitfields.addr = addr.u.high_part & ADDRESS_WATCH_REG_ADDHIGH_MASK;
	addrLo->bitfields.addr =
			(addr.u.low_part >> ADDRESS_WATCH_REG_ADDLOW_SHIFT);

	cntl->bitfields.mode = adw_info->watch_mode[index];
	cntl->bitfields.vmid = (uint32_t) vmid;
	cntl->u32All |= ADDRESS_WATCH_REG_CNTL_ATC_BIT;	/*  for now assume it is an ATC address.  */
	pr_debug("\t\t%20s %08x\n", "set reg mask :", cntl->bitfields.mask);
	pr_debug("\t\t%20s %08x\n", "set reg add high :", addrHi->bitfields.addr);
	pr_debug("\t\t%20s %08x\n", "set reg add low :", addrLo->bitfields.addr);

}

static int dbgdev_address_watch_nodiq(struct kfd_dbgdev *dbgdev,
					struct dbg_address_watch_info *adw_info)
{

	int status = 0;

	union TCP_WATCH_ADDR_H_BITS addrHi;
	union TCP_WATCH_ADDR_L_BITS addrLo;
	union TCP_WATCH_CNTL_BITS cntl;

	unsigned int vmid;
	unsigned int i;

	struct kfd_process_device *pdd;

	do {
		/* taking the vmid for that process on the safe way using pdd */
		pdd = kfd_get_process_device_data(dbgdev->dev,
						adw_info->process);
		if (!pdd) {
			pr_debug("Error! kfd: In func %s >> no PDD available\n", __func__);
			status = -EFAULT;
			break;
		}

		addrHi.u32All = 0;
		addrLo.u32All = 0;
		cntl.u32All = 0;

		vmid = pdd->qpd.vmid;

		if ((adw_info->num_watch_points > MAX_WATCH_ADDRESSES)
		    || (adw_info->num_watch_points == 0)) {
			status = -EINVAL;
			break;
		}

		if ((adw_info->watch_mode == NULL) || (adw_info->watch_address == NULL)) {
			status = -EINVAL;
			break;
		}

		for (i = 0; i < adw_info->num_watch_points; i++) {

			dbgdev_address_watch_set_registers(adw_info, &addrHi, &addrLo, &cntl, i, vmid);

			pr_debug("\t\t%30s\n", "* * * * * * * * * * * * * * * * * *");
			pr_debug("\t\t%20s %08x\n", "register index :", i);
			pr_debug("\t\t%20s %08x\n", "vmid is :", vmid);
			pr_debug("\t\t%20s %08x\n", "Address Low is :", addrLo.bitfields.addr);
			pr_debug("\t\t%20s %08x\n", "Address high is :", addrHi.bitfields.addr);
			pr_debug("\t\t%20s %08x\n", "Address high is :", addrHi.bitfields.addr);
			pr_debug("\t\t%20s %08x\n", "Control Mask is :", cntl.bitfields.mask);
			pr_debug("\t\t%20s %08x\n", "Control Mode is :", cntl.bitfields.mode);
			pr_debug("\t\t%20s %08x\n", "Control Vmid is :", cntl.bitfields.vmid);
			pr_debug("\t\t%20s %08x\n", "Control atc  is :", cntl.bitfields.atc);
			pr_debug("\t\t%30s\n", "* * * * * * * * * * * * * * * * * *");

			pdd->dev->kfd2kgd->address_watch_execute(
							dbgdev->dev->kgd,
							i,
							cntl.u32All,
							addrHi.u32All,
							addrLo.u32All);
		}

	} while (false);

	return status;
}

static int dbgdev_address_watch_diq(struct kfd_dbgdev *dbgdev,
					struct dbg_address_watch_info *adw_info)
{

	int status = 0;
	unsigned int i = 0;
	union TCP_WATCH_ADDR_H_BITS addrHi;
	union TCP_WATCH_ADDR_L_BITS addrLo;
	union TCP_WATCH_CNTL_BITS cntl;

	/* we do not control the vmid in DIQ mode, just a place holder */
	unsigned int vmid = 0;

	struct kfd_mem_obj *mem_obj;
	uint32_t *packet_buff_uint = NULL;

	struct pm4__set_config_reg *packets_vec = NULL;

	size_t ib_size = sizeof(struct pm4__set_config_reg) * 4;

	unsigned int aw_reg_add_dword;

	addrHi.u32All = 0;
	addrLo.u32All = 0;
	cntl.u32All = 0;

	do {

		if ((adw_info->num_watch_points > MAX_WATCH_ADDRESSES) || (adw_info->num_watch_points == 0)) {
			status = -EINVAL;
			break;
		}

		if ((NULL == adw_info->watch_mode) || (NULL == adw_info->watch_address)) {
			status = -EINVAL;
			break;
		}

		status = kfd_gtt_sa_allocate(dbgdev->dev, ib_size, &mem_obj);

		if (status != 0)
			break;

		packet_buff_uint = mem_obj->cpu_ptr;

		memset(packet_buff_uint, 0, ib_size);

		packets_vec = (struct pm4__set_config_reg *) (packet_buff_uint);

		packets_vec[0].header.count = 1;
		packets_vec[0].header.opcode = IT_SET_CONFIG_REG;
		packets_vec[0].header.type = PM4_TYPE_3;
		packets_vec[0].bitfields2.vmid_shift = ADDRESS_WATCH_CNTL_OFFSET;
		packets_vec[0].bitfields2.insert_vmid = 1;
		packets_vec[1].ordinal1 = packets_vec[0].ordinal1;
		packets_vec[1].bitfields2.insert_vmid = 0;
		packets_vec[2].ordinal1 = packets_vec[0].ordinal1;
		packets_vec[2].bitfields2.insert_vmid = 0;
		packets_vec[3].ordinal1 = packets_vec[0].ordinal1;
		packets_vec[3].bitfields2.vmid_shift = ADDRESS_WATCH_CNTL_OFFSET;
		packets_vec[3].bitfields2.insert_vmid = 1;

		for (i = 0; i < adw_info->num_watch_points; i++) {

			dbgdev_address_watch_set_registers(adw_info,
											&addrHi,
											&addrLo,
											&cntl,
											i,
											vmid);

			pr_debug("\t\t%30s\n", "* * * * * * * * * * * * * * * * * *");
			pr_debug("\t\t%20s %08x\n", "register index :", i);
			pr_debug("\t\t%20s %08x\n", "vmid is :", vmid);
			pr_debug("\t\t%20s %p\n", "Add ptr is :", adw_info->watch_address);
			pr_debug("\t\t%20s %08llx\n", "Add     is :", adw_info->watch_address[i]);
			pr_debug("\t\t%20s %08x\n", "Address Low is :", addrLo.bitfields.addr);
			pr_debug("\t\t%20s %08x\n", "Address high is :", addrHi.bitfields.addr);
			pr_debug("\t\t%20s %08x\n", "Control Mask is :", cntl.bitfields.mask);
			pr_debug("\t\t%20s %08x\n", "Control Mode is :", cntl.bitfields.mode);
			pr_debug("\t\t%20s %08x\n", "Control Vmid is :", cntl.bitfields.vmid);
			pr_debug("\t\t%20s %08x\n", "Control atc  is :", cntl.bitfields.atc);
			pr_debug("\t\t%30s\n", "* * * * * * * * * * * * * * * * * *");

			aw_reg_add_dword =
					dbgdev->dev->kfd2kgd
					->address_watch_get_offset(
						dbgdev->dev->kgd,
						i,
						ADDRESS_WATCH_REG_CNTL);

			aw_reg_add_dword /= sizeof(uint32_t);

			packets_vec[0].bitfields2.reg_offset = aw_reg_add_dword - CONFIG_REG_BASE;
			packets_vec[0].reg_data[0] = cntl.u32All;

			aw_reg_add_dword =
					dbgdev->dev->kfd2kgd
					->address_watch_get_offset(
						dbgdev->dev->kgd,
						i,
						ADDRESS_WATCH_REG_ADDR_HI);

			aw_reg_add_dword /= sizeof(uint32_t);

			packets_vec[1].bitfields2.reg_offset = aw_reg_add_dword - CONFIG_REG_BASE;
			packets_vec[1].reg_data[0] = addrHi.u32All;

			aw_reg_add_dword =
					dbgdev->dev->kfd2kgd
					->address_watch_get_offset(
						dbgdev->dev->kgd,
						i,
						ADDRESS_WATCH_REG_ADDR_LO);

			aw_reg_add_dword /= sizeof(uint32_t);

			packets_vec[2].bitfields2.reg_offset = aw_reg_add_dword - CONFIG_REG_BASE;
			packets_vec[2].reg_data[0] = addrLo.u32All;

			/* enable watch flag if address is not zero*/
			if (adw_info->watch_address[i] > 0)
				cntl.bitfields.valid = 1;
			else
				cntl.bitfields.valid = 0;

			aw_reg_add_dword =
					dbgdev->dev->kfd2kgd
					->address_watch_get_offset(
						dbgdev->dev->kgd,
						i,
						ADDRESS_WATCH_REG_CNTL);

			aw_reg_add_dword /= sizeof(uint32_t);

			packets_vec[3].bitfields2.reg_offset = aw_reg_add_dword - CONFIG_REG_BASE;
			packets_vec[3].reg_data[0] = cntl.u32All;

			status = dbgdev_diq_submit_ib(
						dbgdev,
						adw_info->process->pasid,
						mem_obj->gpu_addr,
						packet_buff_uint,
						ib_size);

			if (status != 0) {
				pr_debug("Error! kfd: In func %s >> failed to submit DIQ packet\n", __func__);
				break;
			}

		}

	} while (false);
	if (packet_buff_uint != NULL)
		kfd_gtt_sa_free(dbgdev->dev, mem_obj);

	return status;

}

static int dbgdev_wave_control_set_registers(
				struct dbg_wave_control_info *wac_info,
				union SQ_CMD_BITS *in_reg_sq_cmd,
				union GRBM_GFX_INDEX_BITS *in_reg_gfx_index,
				unsigned int asic_family)
{
	int status = 0;
	union SQ_CMD_BITS reg_sq_cmd;
	union GRBM_GFX_INDEX_BITS reg_gfx_index;

	reg_sq_cmd.u32All = 0;

	reg_gfx_index.u32All = 0;

	switch (wac_info->mode) {
	case HSA_DBG_WAVEMODE_SINGLE:	/*  Send command to single wave  */
		/*limit access to the process waves only,by setting vmid check */
		reg_sq_cmd.bits.check_vmid = 1;
		reg_sq_cmd.bits.simd_id = wac_info->dbgWave_msg.DbgWaveMsg.WaveMsgInfoGen2.ui32.SIMD;
		reg_sq_cmd.bits.wave_id = wac_info->dbgWave_msg.DbgWaveMsg.WaveMsgInfoGen2.ui32.WaveId;
		reg_sq_cmd.bits.mode = SQ_IND_CMD_MODE_SINGLE;

		reg_gfx_index.bits.sh_index = wac_info->dbgWave_msg.DbgWaveMsg.WaveMsgInfoGen2.ui32.ShaderArray;
		reg_gfx_index.bits.se_index = wac_info->dbgWave_msg.DbgWaveMsg.WaveMsgInfoGen2.ui32.ShaderEngine;
		reg_gfx_index.bits.instance_index = wac_info->dbgWave_msg.DbgWaveMsg.WaveMsgInfoGen2.ui32.HSACU;

		break;

	case HSA_DBG_WAVEMODE_BROADCAST_PROCESS:	/*  Send command to all waves with matching VMID  */


		reg_gfx_index.bits.sh_broadcast_writes = 1;
		reg_gfx_index.bits.se_broadcast_writes = 1;
		reg_gfx_index.bits.instance_broadcast_writes = 1;

		reg_sq_cmd.bits.mode = SQ_IND_CMD_MODE_BROADCAST;
		break;

	case HSA_DBG_WAVEMODE_BROADCAST_PROCESS_CU:	/*  Send command to all CU waves with matching VMID  */

		reg_sq_cmd.bits.check_vmid = 1;
		reg_sq_cmd.bits.mode = SQ_IND_CMD_MODE_BROADCAST;

		reg_gfx_index.bits.sh_index = wac_info->dbgWave_msg.DbgWaveMsg.WaveMsgInfoGen2.ui32.ShaderArray;
		reg_gfx_index.bits.se_index = wac_info->dbgWave_msg.DbgWaveMsg.WaveMsgInfoGen2.ui32.ShaderEngine;
		reg_gfx_index.bits.instance_index = wac_info->dbgWave_msg.DbgWaveMsg.WaveMsgInfoGen2.ui32.HSACU;

		break;

	default:
		status = -EINVAL;
		break;
	}

	switch (wac_info->operand) {
	case HSA_DBG_WAVEOP_HALT:
		if (asic_family == CHIP_KAVERI) {
			reg_sq_cmd.bits.cmd = SQ_IND_CMD_CMD_HALT;
			pr_debug("kfd:dbgdev: halting KV\n");
		} else {
			reg_sq_cmd.bits_sethalt.cmd  = SQ_IND_CMD_NEW_SETHALT;
			reg_sq_cmd.bits_sethalt.data = SQ_IND_CMD_DATA_HALT;
			pr_debug("kfd:dbgdev: halting CZ\n");
		}
		break;

	case HSA_DBG_WAVEOP_RESUME:
		if (asic_family == CHIP_KAVERI) {
			reg_sq_cmd.bits.cmd = SQ_IND_CMD_CMD_RESUME;
			pr_debug("kfd:dbgdev: resuming KV\n");
		} else {
			reg_sq_cmd.bits_sethalt.cmd  = SQ_IND_CMD_NEW_SETHALT;
			reg_sq_cmd.bits_sethalt.data = SQ_IND_CMD_DATA_RESUME;
			pr_debug("kfd:dbgdev: resuming CZ\n");
		}
		break;

	case HSA_DBG_WAVEOP_KILL:
		reg_sq_cmd.bits.cmd = SQ_IND_CMD_CMD_KILL;
		break;

	case HSA_DBG_WAVEOP_DEBUG:
		reg_sq_cmd.bits.cmd = SQ_IND_CMD_CMD_DEBUG;
		break;

	case HSA_DBG_WAVEOP_TRAP:
		if (wac_info->trapId < MAX_TRAPID) {
			reg_sq_cmd.bits.cmd = SQ_IND_CMD_CMD_TRAP;
			reg_sq_cmd.bits.trap_id = wac_info->trapId;
		} else {
			status = -EINVAL;
		}
		break;

	default:
		status = -EINVAL;
		break;
	}

	if (status == 0) {
		*in_reg_sq_cmd    = reg_sq_cmd;
		*in_reg_gfx_index = reg_gfx_index;
	}
	return status;

}

static int dbgdev_wave_control_diq(struct kfd_dbgdev *dbgdev,
					struct dbg_wave_control_info *wac_info)
{

	int status = 0;
	union SQ_CMD_BITS reg_sq_cmd;
	union GRBM_GFX_INDEX_BITS reg_gfx_index;
	struct kfd_mem_obj *mem_obj;
	uint32_t *packet_buff_uint = NULL;
	struct pm4__set_config_reg *packets_vec = NULL;
	size_t ib_size = sizeof(struct pm4__set_config_reg) * 3;

	reg_sq_cmd.u32All = 0;
	do {

		status = dbgdev_wave_control_set_registers(wac_info,
				&reg_sq_cmd,
				&reg_gfx_index,
				dbgdev->dev->device_info->asic_family);

		/* we do not control the VMID in DIQ,so reset it to a known value */
		reg_sq_cmd.bits.vm_id = 0;
		if (status != 0)
			break;
		pr_debug("\t\t %30s\n", "* * * * * * * * * * * * * * * * * *");

		pr_debug("\t\t mode      is: %u\n", wac_info->mode);
		pr_debug("\t\t operand   is: %u\n", wac_info->operand);
		pr_debug("\t\t trap id   is: %u\n", wac_info->trapId);
		pr_debug("\t\t msg value is: %u\n", wac_info->dbgWave_msg.DbgWaveMsg.WaveMsgInfoGen2.Value);
		pr_debug("\t\t vmid      is: N/A\n");

		pr_debug("\t\t chk_vmid  is : %u\n", reg_sq_cmd.bitfields.check_vmid);
		pr_debug("\t\t command   is : %u\n", reg_sq_cmd.bitfields.cmd);
		pr_debug("\t\t queue id  is : %u\n", reg_sq_cmd.bitfields.queue_id);
		pr_debug("\t\t simd id   is : %u\n", reg_sq_cmd.bitfields.simd_id);
		pr_debug("\t\t mode      is : %u\n", reg_sq_cmd.bitfields.mode);
		pr_debug("\t\t vm_id     is : %u\n", reg_sq_cmd.bitfields.vm_id);
		pr_debug("\t\t wave_id   is : %u\n", reg_sq_cmd.bitfields.wave_id);

		pr_debug("\t\t ibw       is : %u\n", reg_gfx_index.bitfields.instance_broadcast_writes);
		pr_debug("\t\t ii        is : %u\n", reg_gfx_index.bitfields.instance_index);
		pr_debug("\t\t sebw      is : %u\n", reg_gfx_index.bitfields.se_broadcast_writes);
		pr_debug("\t\t se_ind    is : %u\n", reg_gfx_index.bitfields.se_index);
		pr_debug("\t\t sh_ind    is : %u\n", reg_gfx_index.bitfields.sh_index);
		pr_debug("\t\t sbw       is : %u\n", reg_gfx_index.bitfields.sh_broadcast_writes);

		pr_debug("\t\t %30s\n", "* * * * * * * * * * * * * * * * * *");

		status = kfd_gtt_sa_allocate(dbgdev->dev, ib_size, &mem_obj);

		if (status != 0)
			break;

		packet_buff_uint = mem_obj->cpu_ptr;

		memset(packet_buff_uint, 0, ib_size);

		packets_vec =  (struct pm4__set_config_reg *) packet_buff_uint;
		packets_vec[0].header.count = 1;
		packets_vec[0].header.opcode = IT_SET_UCONFIG_REG;
		packets_vec[0].header.type = PM4_TYPE_3;
		packets_vec[0].bitfields2.reg_offset = GRBM_GFX_INDEX / (sizeof(uint32_t)) - USERCONFIG_REG_BASE;
		packets_vec[0].bitfields2.insert_vmid = 0;
		packets_vec[0].reg_data[0] = reg_gfx_index.u32All;

		packets_vec[1].header.count = 1;
		packets_vec[1].header.opcode = IT_SET_CONFIG_REG;
		packets_vec[1].header.type = PM4_TYPE_3;
		packets_vec[1].bitfields2.reg_offset = SQ_CMD / (sizeof(uint32_t)) - CONFIG_REG_BASE;
		packets_vec[1].bitfields2.vmid_shift = SQ_CMD_VMID_OFFSET;
		packets_vec[1].bitfields2.insert_vmid = 1;
		packets_vec[1].reg_data[0] = reg_sq_cmd.u32All;

		/* Restore the GRBM_GFX_INDEX register */

		reg_gfx_index.u32All = 0;
		reg_gfx_index.bits.sh_broadcast_writes = 1;
		reg_gfx_index.bits.instance_broadcast_writes = 1;
		reg_gfx_index.bits.se_broadcast_writes = 1;


		packets_vec[2].ordinal1 = packets_vec[0].ordinal1;
		packets_vec[2].bitfields2.reg_offset = GRBM_GFX_INDEX / (sizeof(uint32_t)) - USERCONFIG_REG_BASE;
		packets_vec[2].bitfields2.insert_vmid = 0;
		packets_vec[2].reg_data[0] = reg_gfx_index.u32All;

		status = dbgdev_diq_submit_ib(
				dbgdev,
				wac_info->process->pasid,
				mem_obj->gpu_addr,
				packet_buff_uint,
				ib_size);

		if (status != 0)
			pr_debug("%s\n", " Critical Error ! Submit diq packet failed ");

	} while (false);

	if (packet_buff_uint != NULL)
		kfd_gtt_sa_free(dbgdev->dev, mem_obj);

	return status;
}

static int dbgdev_wave_control_nodiq(struct kfd_dbgdev *dbgdev,
					struct dbg_wave_control_info *wac_info)
{
	int status = 0;
	unsigned int vmid = 0xffff;
	union SQ_CMD_BITS reg_sq_cmd;
	union GRBM_GFX_INDEX_BITS reg_gfx_index;

	struct kfd_process_device *pdd = NULL;

	reg_sq_cmd.u32All = 0;
	status = 0;

	/* taking the VMID for that process on the safe way using PDD */
	pdd = kfd_get_process_device_data(dbgdev->dev, wac_info->process);

	if (pdd) {
		status = dbgdev_wave_control_set_registers(wac_info,
				&reg_sq_cmd,
				&reg_gfx_index,
				dbgdev->dev->device_info->asic_family);
		if (status == 0) {

			/* for non DIQ we need to patch the VMID: */

			vmid = pdd->qpd.vmid;
			reg_sq_cmd.bits.vm_id = vmid;

			pr_debug("\t\t %30s\n", "* * * * * * * * * * * * * * * * * *");

			pr_debug("\t\t mode      is: %u\n", wac_info->mode);
			pr_debug("\t\t operand   is: %u\n", wac_info->operand);
			pr_debug("\t\t trap id   is: %u\n", wac_info->trapId);
			pr_debug("\t\t msg value is: %u\n", wac_info->dbgWave_msg.DbgWaveMsg.WaveMsgInfoGen2.Value);
			pr_debug("\t\t vmid      is: %u\n", vmid);

			pr_debug("\t\t chk_vmid  is : %u\n", reg_sq_cmd.bitfields.check_vmid);
			pr_debug("\t\t command   is : %u\n", reg_sq_cmd.bitfields.cmd);
			pr_debug("\t\t queue id  is : %u\n", reg_sq_cmd.bitfields.queue_id);
			pr_debug("\t\t simd id   is : %u\n", reg_sq_cmd.bitfields.simd_id);
			pr_debug("\t\t mode      is : %u\n", reg_sq_cmd.bitfields.mode);
			pr_debug("\t\t vm_id     is : %u\n", reg_sq_cmd.bitfields.vm_id);
			pr_debug("\t\t wave_id   is : %u\n", reg_sq_cmd.bitfields.wave_id);

			pr_debug("\t\t ibw       is : %u\n", reg_gfx_index.bitfields.instance_broadcast_writes);
			pr_debug("\t\t ii        is : %u\n", reg_gfx_index.bitfields.instance_index);
			pr_debug("\t\t sebw      is : %u\n", reg_gfx_index.bitfields.se_broadcast_writes);
			pr_debug("\t\t se_ind    is : %u\n", reg_gfx_index.bitfields.se_index);
			pr_debug("\t\t sh_ind    is : %u\n", reg_gfx_index.bitfields.sh_index);
			pr_debug("\t\t sbw       is : %u\n", reg_gfx_index.bitfields.sh_broadcast_writes);

			pr_debug("\t\t %30s\n", "* * * * * * * * * * * * * * * * * *");

			dbgdev->dev->kfd2kgd
				->wave_control_execute(dbgdev->dev->kgd,
							reg_gfx_index.u32All,
							reg_sq_cmd.u32All);
		} else {
			status = -EINVAL;
		}
	} else {
		status = -EFAULT;
	}

	return status;

}

int dbgdev_wave_reset_wavefronts(struct kfd_dev *dev, struct kfd_process *p)
{
	int status = 0;
	unsigned int vmid;
	union SQ_CMD_BITS reg_sq_cmd;
	union GRBM_GFX_INDEX_BITS reg_gfx_index;
	struct kfd_process_device *pdd;
	struct dbg_wave_control_info wac_info;
	int temp;
	int first_vmid_to_scan = 8;
	int last_vmid_to_scan = 15;

	first_vmid_to_scan = ffs(dev->shared_resources.compute_vmid_bitmap) - 1;
	temp = dev->shared_resources.compute_vmid_bitmap >> first_vmid_to_scan;
	last_vmid_to_scan = first_vmid_to_scan + ffz(temp);

	reg_sq_cmd.u32All = 0;
	status = 0;

	wac_info.mode = HSA_DBG_WAVEMODE_BROADCAST_PROCESS;
	wac_info.operand = HSA_DBG_WAVEOP_KILL;

	pr_debug("Killing all process wavefronts\n");

	/* Scan all registers in the range ATC_VMID8_PASID_MAPPING ..
	 * ATC_VMID15_PASID_MAPPING
	 * to check which VMID the current process is mapped to. */

	for (vmid = first_vmid_to_scan; vmid <= last_vmid_to_scan; vmid++) {
		if (dev->kfd2kgd->get_atc_vmid_pasid_mapping_valid
				(dev->kgd, vmid)) {
			if (dev->kfd2kgd->get_atc_vmid_pasid_mapping_valid
					(dev->kgd, vmid) == p->pasid) {
				pr_debug("Killing wave fronts of vmid %d and pasid %d\n",
						vmid, p->pasid);
				break;
			}
		}
	}

	if (vmid > last_vmid_to_scan) {
		pr_err("amdkfd: didn't found vmid for pasid (%d)\n", p->pasid);
		return -EFAULT;
	}

	/* taking the VMID for that process on the safe way using PDD */
	pdd = kfd_get_process_device_data(dev, p);
	if (!pdd)
		return -EFAULT;

	status = dbgdev_wave_control_set_registers(&wac_info, &reg_sq_cmd,
			&reg_gfx_index, dev->device_info->asic_family);
	if (status != 0)
		return -EINVAL;

	/* for non DIQ we need to patch the VMID: */
	reg_sq_cmd.bits.vm_id = vmid;

	dev->kfd2kgd->wave_control_execute(dev->kgd,
					reg_gfx_index.u32All,
					reg_sq_cmd.u32All);

	return 0;
}

void kfd_dbgdev_init(struct kfd_dbgdev *pdbgdev, struct kfd_dev *pdev,
			DBGDEV_TYPE type)
{
	pdbgdev->dev = pdev;
	pdbgdev->kq = NULL;
	pdbgdev->type = type;
	pdbgdev->pqm = NULL;
	switch (type) {
	case DBGDEV_TYPE_NODIQ:
		pdbgdev->dbgdev_register = dbgdev_register_nodiq;
		pdbgdev->dbgdev_unregister = dbgdev_unregister_nodiq;
		pdbgdev->dbgdev_wave_control = dbgdev_wave_control_nodiq;
		pdbgdev->dbgdev_address_watch = dbgdev_address_watch_nodiq;
		break;
	case DBGDEV_TYPE_DIQ:
	default:

		pdbgdev->dbgdev_register = dbgdev_register_diq;
		pdbgdev->dbgdev_unregister = dbgdev_unregister_diq;
		pdbgdev->dbgdev_wave_control =  dbgdev_wave_control_diq;
		pdbgdev->dbgdev_address_watch = dbgdev_address_watch_diq;

		break;
	}

}
