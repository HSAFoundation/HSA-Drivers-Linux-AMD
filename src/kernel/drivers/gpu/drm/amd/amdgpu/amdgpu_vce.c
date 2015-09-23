/*
 * Copyright 2013 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * Authors: Christian König <christian.koenig@amd.com>
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <drm/drmP.h>
#include <drm/drm.h>

#include "amdgpu.h"
#include "amdgpu_pm.h"
#include "amdgpu_vce.h"
#include "cikd.h"

/* 1 second timeout */
#define VCE_IDLE_TIMEOUT_MS	1000

/* Firmware Names */
#ifdef CONFIG_DRM_AMDGPU_CIK
#define FIRMWARE_BONAIRE	"radeon/bonaire_vce.bin"
#define FIRMWARE_KABINI 	"radeon/kabini_vce.bin"
#define FIRMWARE_KAVERI 	"radeon/kaveri_vce.bin"
#define FIRMWARE_HAWAII 	"radeon/hawaii_vce.bin"
#define FIRMWARE_MULLINS	"radeon/mullins_vce.bin"
#endif
#define FIRMWARE_TONGA		"radeon/tonga_vce.bin"
#define FIRMWARE_CARRIZO	"radeon/carrizo_vce.bin"

#ifdef CONFIG_DRM_AMDGPU_CIK
MODULE_FIRMWARE(FIRMWARE_BONAIRE);
MODULE_FIRMWARE(FIRMWARE_KABINI);
MODULE_FIRMWARE(FIRMWARE_KAVERI);
MODULE_FIRMWARE(FIRMWARE_HAWAII);
MODULE_FIRMWARE(FIRMWARE_MULLINS);
#endif
MODULE_FIRMWARE(FIRMWARE_TONGA);
MODULE_FIRMWARE(FIRMWARE_CARRIZO);

static void amdgpu_vce_idle_work_handler(struct work_struct *work);

/**
 * amdgpu_vce_init - allocate memory, load vce firmware
 *
 * @adev: amdgpu_device pointer
 *
 * First step to get VCE online, allocate memory and load the firmware
 */
int amdgpu_vce_sw_init(struct amdgpu_device *adev)
{
	unsigned long size;
	const char *fw_name;
	const struct common_firmware_header *hdr;
	unsigned ucode_version, version_major, version_minor, binary_id;
	int i, r;

	INIT_DELAYED_WORK(&adev->vce.idle_work, amdgpu_vce_idle_work_handler);

	switch (adev->asic_type) {
#ifdef CONFIG_DRM_AMDGPU_CIK
	case CHIP_BONAIRE:
		fw_name = FIRMWARE_BONAIRE;
		break;
	case CHIP_KAVERI:
		fw_name = FIRMWARE_KAVERI;
		break;
	case CHIP_KABINI:
		fw_name = FIRMWARE_KABINI;
		break;
	case CHIP_HAWAII:
		fw_name = FIRMWARE_HAWAII;
		break;
	case CHIP_MULLINS:
		fw_name = FIRMWARE_MULLINS;
		break;
#endif
	case CHIP_TONGA:
		fw_name = FIRMWARE_TONGA;
		break;
	case CHIP_CARRIZO:
		fw_name = FIRMWARE_CARRIZO;
		break;

	default:
		return -EINVAL;
	}

	r = request_firmware(&adev->vce.fw, fw_name, adev->dev);
	if (r) {
		dev_err(adev->dev, "amdgpu_vce: Can't load firmware \"%s\"\n",
			fw_name);
		return r;
	}

	r = amdgpu_ucode_validate(adev->vce.fw);
	if (r) {
		dev_err(adev->dev, "amdgpu_vce: Can't validate firmware \"%s\"\n",
			fw_name);
		release_firmware(adev->vce.fw);
		adev->vce.fw = NULL;
		return r;
	}

	hdr = (const struct common_firmware_header *)adev->vce.fw->data;

	ucode_version = le32_to_cpu(hdr->ucode_version);
	version_major = (ucode_version >> 20) & 0xfff;
	version_minor = (ucode_version >> 8) & 0xfff;
	binary_id = ucode_version & 0xff;
	DRM_INFO("Found VCE firmware Version: %hhd.%hhd Binary ID: %hhd\n",
		version_major, version_minor, binary_id);
	adev->vce.fw_version = ((version_major << 24) | (version_minor << 16) |
				(binary_id << 8));

	/* allocate firmware, stack and heap BO */

	size = AMDGPU_GPU_PAGE_ALIGN(le32_to_cpu(hdr->ucode_size_bytes)) +
	       AMDGPU_VCE_STACK_SIZE + AMDGPU_VCE_HEAP_SIZE;
	r = amdgpu_bo_create(adev, size, PAGE_SIZE, true,
			     AMDGPU_GEM_DOMAIN_VRAM, 0, NULL, &adev->vce.vcpu_bo);
	if (r) {
		dev_err(adev->dev, "(%d) failed to allocate VCE bo\n", r);
		return r;
	}

	r = amdgpu_bo_reserve(adev->vce.vcpu_bo, false);
	if (r) {
		amdgpu_bo_unref(&adev->vce.vcpu_bo);
		dev_err(adev->dev, "(%d) failed to reserve VCE bo\n", r);
		return r;
	}

	r = amdgpu_bo_pin(adev->vce.vcpu_bo, AMDGPU_GEM_DOMAIN_VRAM,
			  &adev->vce.gpu_addr);
	amdgpu_bo_unreserve(adev->vce.vcpu_bo);
	if (r) {
		amdgpu_bo_unref(&adev->vce.vcpu_bo);
		dev_err(adev->dev, "(%d) VCE bo pin failed\n", r);
		return r;
	}

	for (i = 0; i < AMDGPU_MAX_VCE_HANDLES; ++i) {
		atomic_set(&adev->vce.handles[i], 0);
		adev->vce.filp[i] = NULL;
	}

	return 0;
}

/**
 * amdgpu_vce_fini - free memory
 *
 * @adev: amdgpu_device pointer
 *
 * Last step on VCE teardown, free firmware memory
 */
int amdgpu_vce_sw_fini(struct amdgpu_device *adev)
{
	if (adev->vce.vcpu_bo == NULL)
		return 0;

	amdgpu_bo_unref(&adev->vce.vcpu_bo);

	amdgpu_ring_fini(&adev->vce.ring[0]);
	amdgpu_ring_fini(&adev->vce.ring[1]);

	release_firmware(adev->vce.fw);

	return 0;
}

/**
 * amdgpu_vce_suspend - unpin VCE fw memory
 *
 * @adev: amdgpu_device pointer
 *
 */
int amdgpu_vce_suspend(struct amdgpu_device *adev)
{
	int i;

	if (adev->vce.vcpu_bo == NULL)
		return 0;

	for (i = 0; i < AMDGPU_MAX_VCE_HANDLES; ++i)
		if (atomic_read(&adev->vce.handles[i]))
			break;

	if (i == AMDGPU_MAX_VCE_HANDLES)
		return 0;

	/* TODO: suspending running encoding sessions isn't supported */
	return -EINVAL;
}

/**
 * amdgpu_vce_resume - pin VCE fw memory
 *
 * @adev: amdgpu_device pointer
 *
 */
int amdgpu_vce_resume(struct amdgpu_device *adev)
{
	void *cpu_addr;
	const struct common_firmware_header *hdr;
	unsigned offset;
	int r;

	if (adev->vce.vcpu_bo == NULL)
		return -EINVAL;

	r = amdgpu_bo_reserve(adev->vce.vcpu_bo, false);
	if (r) {
		dev_err(adev->dev, "(%d) failed to reserve VCE bo\n", r);
		return r;
	}

	r = amdgpu_bo_kmap(adev->vce.vcpu_bo, &cpu_addr);
	if (r) {
		amdgpu_bo_unreserve(adev->vce.vcpu_bo);
		dev_err(adev->dev, "(%d) VCE map failed\n", r);
		return r;
	}

	hdr = (const struct common_firmware_header *)adev->vce.fw->data;
	offset = le32_to_cpu(hdr->ucode_array_offset_bytes);
	memcpy(cpu_addr, (adev->vce.fw->data) + offset,
		(adev->vce.fw->size) - offset);

	amdgpu_bo_kunmap(adev->vce.vcpu_bo);

	amdgpu_bo_unreserve(adev->vce.vcpu_bo);

	return 0;
}

/**
 * amdgpu_vce_idle_work_handler - power off VCE
 *
 * @work: pointer to work structure
 *
 * power of VCE when it's not used any more
 */
static void amdgpu_vce_idle_work_handler(struct work_struct *work)
{
	struct amdgpu_device *adev =
		container_of(work, struct amdgpu_device, vce.idle_work.work);

	if ((amdgpu_fence_count_emitted(&adev->vce.ring[0]) == 0) &&
	    (amdgpu_fence_count_emitted(&adev->vce.ring[1]) == 0)) {
		if (adev->pm.dpm_enabled) {
			amdgpu_dpm_enable_vce(adev, false);
		} else {
			amdgpu_asic_set_vce_clocks(adev, 0, 0);
		}
	} else {
		schedule_delayed_work(&adev->vce.idle_work,
				      msecs_to_jiffies(VCE_IDLE_TIMEOUT_MS));
	}
}

/**
 * amdgpu_vce_note_usage - power up VCE
 *
 * @adev: amdgpu_device pointer
 *
 * Make sure VCE is powerd up when we want to use it
 */
static void amdgpu_vce_note_usage(struct amdgpu_device *adev)
{
	bool streams_changed = false;
	bool set_clocks = !cancel_delayed_work_sync(&adev->vce.idle_work);
	set_clocks &= schedule_delayed_work(&adev->vce.idle_work,
					    msecs_to_jiffies(VCE_IDLE_TIMEOUT_MS));

	if (adev->pm.dpm_enabled) {
		/* XXX figure out if the streams changed */
		streams_changed = false;
	}

	if (set_clocks || streams_changed) {
		if (adev->pm.dpm_enabled) {
			amdgpu_dpm_enable_vce(adev, true);
		} else {
			amdgpu_asic_set_vce_clocks(adev, 53300, 40000);
		}
	}
}

/**
 * amdgpu_vce_free_handles - free still open VCE handles
 *
 * @adev: amdgpu_device pointer
 * @filp: drm file pointer
 *
 * Close all VCE handles still open by this file pointer
 */
void amdgpu_vce_free_handles(struct amdgpu_device *adev, struct drm_file *filp)
{
	struct amdgpu_ring *ring = &adev->vce.ring[0];
	int i, r;
	for (i = 0; i < AMDGPU_MAX_VCE_HANDLES; ++i) {
		uint32_t handle = atomic_read(&adev->vce.handles[i]);
		if (!handle || adev->vce.filp[i] != filp)
			continue;

		amdgpu_vce_note_usage(adev);

		r = amdgpu_vce_get_destroy_msg(ring, handle, NULL);
		if (r)
			DRM_ERROR("Error destroying VCE handle (%d)!\n", r);

		adev->vce.filp[i] = NULL;
		atomic_set(&adev->vce.handles[i], 0);
	}
}

/**
 * amdgpu_vce_get_create_msg - generate a VCE create msg
 *
 * @adev: amdgpu_device pointer
 * @ring: ring we should submit the msg to
 * @handle: VCE session handle to use
 * @fence: optional fence to return
 *
 * Open up a stream for HW test
 */
int amdgpu_vce_get_create_msg(struct amdgpu_ring *ring, uint32_t handle,
			      struct amdgpu_fence **fence)
{
	const unsigned ib_size_dw = 1024;
	struct amdgpu_ib ib;
	uint64_t dummy;
	int i, r;

	r = amdgpu_ib_get(ring, NULL, ib_size_dw * 4, &ib);
	if (r) {
		DRM_ERROR("amdgpu: failed to get ib (%d).\n", r);
		return r;
	}

	dummy = ib.gpu_addr + 1024;

	/* stitch together an VCE create msg */
	ib.length_dw = 0;
	ib.ptr[ib.length_dw++] = 0x0000000c; /* len */
	ib.ptr[ib.length_dw++] = 0x00000001; /* session cmd */
	ib.ptr[ib.length_dw++] = handle;

	ib.ptr[ib.length_dw++] = 0x00000030; /* len */
	ib.ptr[ib.length_dw++] = 0x01000001; /* create cmd */
	ib.ptr[ib.length_dw++] = 0x00000000;
	ib.ptr[ib.length_dw++] = 0x00000042;
	ib.ptr[ib.length_dw++] = 0x0000000a;
	ib.ptr[ib.length_dw++] = 0x00000001;
	ib.ptr[ib.length_dw++] = 0x00000080;
	ib.ptr[ib.length_dw++] = 0x00000060;
	ib.ptr[ib.length_dw++] = 0x00000100;
	ib.ptr[ib.length_dw++] = 0x00000100;
	ib.ptr[ib.length_dw++] = 0x0000000c;
	ib.ptr[ib.length_dw++] = 0x00000000;

	ib.ptr[ib.length_dw++] = 0x00000014; /* len */
	ib.ptr[ib.length_dw++] = 0x05000005; /* feedback buffer */
	ib.ptr[ib.length_dw++] = upper_32_bits(dummy);
	ib.ptr[ib.length_dw++] = dummy;
	ib.ptr[ib.length_dw++] = 0x00000001;

	for (i = ib.length_dw; i < ib_size_dw; ++i)
		ib.ptr[i] = 0x0;

	r = amdgpu_ib_schedule(ring->adev, 1, &ib, AMDGPU_FENCE_OWNER_UNDEFINED);
	if (r) {
		DRM_ERROR("amdgpu: failed to schedule ib (%d).\n", r);
	}

	if (fence)
		*fence = amdgpu_fence_ref(ib.fence);

	amdgpu_ib_free(ring->adev, &ib);

	return r;
}

/**
 * amdgpu_vce_get_destroy_msg - generate a VCE destroy msg
 *
 * @adev: amdgpu_device pointer
 * @ring: ring we should submit the msg to
 * @handle: VCE session handle to use
 * @fence: optional fence to return
 *
 * Close up a stream for HW test or if userspace failed to do so
 */
int amdgpu_vce_get_destroy_msg(struct amdgpu_ring *ring, uint32_t handle,
			       struct amdgpu_fence **fence)
{
	const unsigned ib_size_dw = 1024;
	struct amdgpu_ib ib;
	uint64_t dummy;
	int i, r;

	r = amdgpu_ib_get(ring, NULL, ib_size_dw * 4, &ib);
	if (r) {
		DRM_ERROR("amdgpu: failed to get ib (%d).\n", r);
		return r;
	}

	dummy = ib.gpu_addr + 1024;

	/* stitch together an VCE destroy msg */
	ib.length_dw = 0;
	ib.ptr[ib.length_dw++] = 0x0000000c; /* len */
	ib.ptr[ib.length_dw++] = 0x00000001; /* session cmd */
	ib.ptr[ib.length_dw++] = handle;

	ib.ptr[ib.length_dw++] = 0x00000014; /* len */
	ib.ptr[ib.length_dw++] = 0x05000005; /* feedback buffer */
	ib.ptr[ib.length_dw++] = upper_32_bits(dummy);
	ib.ptr[ib.length_dw++] = dummy;
	ib.ptr[ib.length_dw++] = 0x00000001;

	ib.ptr[ib.length_dw++] = 0x00000008; /* len */
	ib.ptr[ib.length_dw++] = 0x02000001; /* destroy cmd */

	for (i = ib.length_dw; i < ib_size_dw; ++i)
		ib.ptr[i] = 0x0;

	r = amdgpu_ib_schedule(ring->adev, 1, &ib, AMDGPU_FENCE_OWNER_UNDEFINED);
	if (r) {
		DRM_ERROR("amdgpu: failed to schedule ib (%d).\n", r);
	}

	if (fence)
		*fence = amdgpu_fence_ref(ib.fence);

	amdgpu_ib_free(ring->adev, &ib);

	return r;
}

/**
 * amdgpu_vce_cs_reloc - command submission relocation
 *
 * @p: parser context
 * @lo: address of lower dword
 * @hi: address of higher dword
 *
 * Patch relocation inside command stream with real buffer address
 */
int amdgpu_vce_cs_reloc(struct amdgpu_cs_parser *p, uint32_t ib_idx, int lo, int hi)
{
	struct amdgpu_bo_va_mapping *mapping;
	struct amdgpu_ib *ib = &p->ibs[ib_idx];
	struct amdgpu_bo *bo;
	uint64_t addr;

	addr = ((uint64_t)amdgpu_get_ib_value(p, ib_idx, lo)) |
	       ((uint64_t)amdgpu_get_ib_value(p, ib_idx, hi)) << 32;

	mapping = amdgpu_cs_find_mapping(p, addr, &bo);
	if (mapping == NULL) {
		DRM_ERROR("Can't find BO for addr 0x%010Lx %d %d\n",
			  addr, lo, hi);
		return -EINVAL;
	}

	addr -= ((uint64_t)mapping->it.start) * AMDGPU_GPU_PAGE_SIZE;
	addr += amdgpu_bo_gpu_offset(bo);

	ib->ptr[lo] = addr & 0xFFFFFFFF;
	ib->ptr[hi] = addr >> 32;

	return 0;
}

/**
 * amdgpu_vce_cs_parse - parse and validate the command stream
 *
 * @p: parser context
 *
 */
int amdgpu_vce_ring_parse_cs(struct amdgpu_cs_parser *p, uint32_t ib_idx)
{
	uint32_t handle = 0;
	bool destroy = false;
	int i, r, idx = 0;
	struct amdgpu_ib *ib = &p->ibs[ib_idx];

	amdgpu_vce_note_usage(p->adev);

	while (idx < ib->length_dw) {
		uint32_t len = amdgpu_get_ib_value(p, ib_idx, idx);
		uint32_t cmd = amdgpu_get_ib_value(p, ib_idx, idx + 1);

		if ((len < 8) || (len & 3)) {
			DRM_ERROR("invalid VCE command length (%d)!\n", len);
			return -EINVAL;
		}

		switch (cmd) {
		case 0x00000001: // session
			handle = amdgpu_get_ib_value(p, ib_idx, idx + 2);
			break;

		case 0x00000002: // task info
		case 0x01000001: // create
		case 0x04000001: // config extension
		case 0x04000002: // pic control
		case 0x04000005: // rate control
		case 0x04000007: // motion estimation
		case 0x04000008: // rdo
		case 0x04000009: // vui
		case 0x05000002: // auxiliary buffer
			break;

		case 0x03000001: // encode
			r = amdgpu_vce_cs_reloc(p, ib_idx, idx + 10, idx + 9);
			if (r)
				return r;

			r = amdgpu_vce_cs_reloc(p, ib_idx, idx + 12, idx + 11);
			if (r)
				return r;
			break;

		case 0x02000001: // destroy
			destroy = true;
			break;

		case 0x05000001: // context buffer
		case 0x05000004: // video bitstream buffer
		case 0x05000005: // feedback buffer
			r = amdgpu_vce_cs_reloc(p, ib_idx, idx + 3, idx + 2);
			if (r)
				return r;
			break;

		default:
			DRM_ERROR("invalid VCE command (0x%x)!\n", cmd);
			return -EINVAL;
		}

		idx += len / 4;
	}

	if (destroy) {
		/* IB contains a destroy msg, free the handle */
		for (i = 0; i < AMDGPU_MAX_VCE_HANDLES; ++i)
			atomic_cmpxchg(&p->adev->vce.handles[i], handle, 0);

		return 0;
	}

	/* create or encode, validate the handle */
	for (i = 0; i < AMDGPU_MAX_VCE_HANDLES; ++i) {
		if (atomic_read(&p->adev->vce.handles[i]) == handle)
			return 0;
	}

	/* handle not found try to alloc a new one */
	for (i = 0; i < AMDGPU_MAX_VCE_HANDLES; ++i) {
		if (!atomic_cmpxchg(&p->adev->vce.handles[i], 0, handle)) {
			p->adev->vce.filp[i] = p->filp;
			return 0;
		}
	}

	DRM_ERROR("No more free VCE handles!\n");

	return -EINVAL;
}

/**
 * amdgpu_vce_ring_emit_semaphore - emit a semaphore command
 *
 * @ring: engine to use
 * @semaphore: address of semaphore
 * @emit_wait: true=emit wait, false=emit signal
 *
 */
bool amdgpu_vce_ring_emit_semaphore(struct amdgpu_ring *ring,
				    struct amdgpu_semaphore *semaphore,
				    bool emit_wait)
{
	uint64_t addr = semaphore->gpu_addr;

	amdgpu_ring_write(ring, VCE_CMD_SEMAPHORE);
	amdgpu_ring_write(ring, (addr >> 3) & 0x000FFFFF);
	amdgpu_ring_write(ring, (addr >> 23) & 0x000FFFFF);
	amdgpu_ring_write(ring, 0x01003000 | (emit_wait ? 1 : 0));
	if (!emit_wait)
		amdgpu_ring_write(ring, VCE_CMD_END);

	return true;
}

/**
 * amdgpu_vce_ring_emit_ib - execute indirect buffer
 *
 * @ring: engine to use
 * @ib: the IB to execute
 *
 */
void amdgpu_vce_ring_emit_ib(struct amdgpu_ring *ring, struct amdgpu_ib *ib)
{
	amdgpu_ring_write(ring, VCE_CMD_IB);
	amdgpu_ring_write(ring, lower_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, ib->length_dw);
}

/**
 * amdgpu_vce_ring_emit_fence - add a fence command to the ring
 *
 * @ring: engine to use
 * @fence: the fence
 *
 */
void amdgpu_vce_ring_emit_fence(struct amdgpu_ring *ring, u64 addr, u64 seq,
				bool write64bits)
{
	WARN_ON(write64bits);

	amdgpu_ring_write(ring, VCE_CMD_FENCE);
	amdgpu_ring_write(ring, addr);
	amdgpu_ring_write(ring, upper_32_bits(addr));
	amdgpu_ring_write(ring, seq);
	amdgpu_ring_write(ring, VCE_CMD_TRAP);
	amdgpu_ring_write(ring, VCE_CMD_END);
}

/**
 * amdgpu_vce_ring_test_ring - test if VCE ring is working
 *
 * @ring: the engine to test on
 *
 */
int amdgpu_vce_ring_test_ring(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t rptr = amdgpu_ring_get_rptr(ring);
	unsigned i;
	int r;

	r = amdgpu_ring_lock(ring, 16);
	if (r) {
		DRM_ERROR("amdgpu: vce failed to lock ring %d (%d).\n",
			  ring->idx, r);
		return r;
	}
	amdgpu_ring_write(ring, VCE_CMD_END);
	amdgpu_ring_unlock_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		if (amdgpu_ring_get_rptr(ring) != rptr)
			break;
		DRM_UDELAY(1);
	}

	if (i < adev->usec_timeout) {
		DRM_INFO("ring test on %d succeeded in %d usecs\n",
			 ring->idx, i);
	} else {
		DRM_ERROR("amdgpu: ring %d test failed\n",
			  ring->idx);
		r = -ETIMEDOUT;
	}

	return r;
}

/**
 * amdgpu_vce_ring_test_ib - test if VCE IBs are working
 *
 * @ring: the engine to test on
 *
 */
int amdgpu_vce_ring_test_ib(struct amdgpu_ring *ring)
{
	struct amdgpu_fence *fence = NULL;
	int r;

	r = amdgpu_vce_get_create_msg(ring, 1, NULL);
	if (r) {
		DRM_ERROR("amdgpu: failed to get create msg (%d).\n", r);
		goto error;
	}

	r = amdgpu_vce_get_destroy_msg(ring, 1, &fence);
	if (r) {
		DRM_ERROR("amdgpu: failed to get destroy ib (%d).\n", r);
		goto error;
	}

	r = amdgpu_fence_wait(fence, false);
	if (r) {
		DRM_ERROR("amdgpu: fence wait failed (%d).\n", r);
	} else {
		DRM_INFO("ib test on ring %d succeeded\n", ring->idx);
	}
error:
	amdgpu_fence_unref(&fence);
	return r;
}
