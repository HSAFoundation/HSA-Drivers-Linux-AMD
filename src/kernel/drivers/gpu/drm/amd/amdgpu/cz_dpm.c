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

#include <linux/firmware.h>
#include <linux/seq_file.h>
#include "drmP.h"
#include "amdgpu.h"
#include "amdgpu_pm.h"
#include "amdgpu_atombios.h"
#include "vid.h"
#include "vi_dpm.h"
#include "amdgpu_dpm.h"
#include "cz_dpm.h"
#include "cz_ppsmc.h"
#include "atom.h"

#include "smu/smu_8_0_d.h"
#include "smu/smu_8_0_sh_mask.h"
#include "gca/gfx_8_0_d.h"
#include "gca/gfx_8_0_sh_mask.h"
#include "gmc/gmc_8_1_d.h"
#include "bif/bif_5_1_d.h"
#include "gfx_v8_0.h"

static struct cz_ps *cz_get_ps(struct amdgpu_ps *rps)
{
	struct cz_ps *ps = rps->ps_priv;

	return ps;
}

static struct cz_power_info *cz_get_pi(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = adev->pm.dpm.priv;

	return pi;
}

static uint16_t cz_convert_8bit_index_to_voltage(struct amdgpu_device *adev,
							uint16_t voltage)
{
	uint16_t tmp = 6200 - voltage * 25;

	return tmp;
}

static void cz_construct_max_power_limits_table(struct amdgpu_device *adev,
				struct amdgpu_clock_and_voltage_limits *table)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_clock_voltage_dependency_table *dep_table =
		&adev->pm.dpm.dyn_state.vddc_dependency_on_sclk;

	if (dep_table->count > 0) {
		table->sclk = dep_table->entries[dep_table->count - 1].clk;
		table->vddc = cz_convert_8bit_index_to_voltage(adev,
				dep_table->entries[dep_table->count - 1].v);
	}

	table->mclk = pi->sys_info.nbp_memory_clock[0];

}

union igp_info {
	struct _ATOM_INTEGRATED_SYSTEM_INFO info;
	struct _ATOM_INTEGRATED_SYSTEM_INFO_V1_7 info_7;
	struct _ATOM_INTEGRATED_SYSTEM_INFO_V1_8 info_8;
	struct _ATOM_INTEGRATED_SYSTEM_INFO_V1_9 info_9;
};

static int cz_parse_sys_info_table(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_mode_info *mode_info = &adev->mode_info;
	int index = GetIndexIntoMasterTable(DATA, IntegratedSystemInfo);
	union igp_info *igp_info;
	u8 frev, crev;
	u16 data_offset;
	int i = 0;

	if (amdgpu_atom_parse_data_header(mode_info->atom_context, index, NULL,
				   &frev, &crev, &data_offset)) {
		igp_info = (union igp_info *)(mode_info->atom_context->bios +
					      data_offset);

		if (crev != 9) {
			DRM_ERROR("Unsupported IGP table: %d %d\n", frev, crev);
			return -EINVAL;
		}
		pi->sys_info.bootup_sclk =
			le32_to_cpu(igp_info->info_9.ulBootUpEngineClock);
		pi->sys_info.bootup_uma_clk =
			le32_to_cpu(igp_info->info_9.ulBootUpUMAClock);
		pi->sys_info.dentist_vco_freq =
			le32_to_cpu(igp_info->info_9.ulDentistVCOFreq);
		pi->sys_info.bootup_nb_voltage_index =
			le16_to_cpu(igp_info->info_9.usBootUpNBVoltage);

		if (igp_info->info_9.ucHtcTmpLmt == 0)
			pi->sys_info.htc_tmp_lmt = 203;
		else
			pi->sys_info.htc_tmp_lmt = igp_info->info_9.ucHtcTmpLmt;

		if (igp_info->info_9.ucHtcHystLmt == 0)
			pi->sys_info.htc_hyst_lmt = 5;
		else
			pi->sys_info.htc_hyst_lmt = igp_info->info_9.ucHtcHystLmt;

		if (pi->sys_info.htc_tmp_lmt <= pi->sys_info.htc_hyst_lmt) {
			DRM_ERROR("The htcTmpLmt should be larger than htcHystLmt.\n");
			return -EINVAL;
		}

		if (le32_to_cpu(igp_info->info_9.ulSystemConfig) & (1 << 3) &&
				pi->enable_nb_ps_policy)
			pi->sys_info.nb_dpm_enable = true;
		else
			pi->sys_info.nb_dpm_enable = false;

		for (i = 0; i < CZ_NUM_NBPSTATES; i++) {
			if (i < CZ_NUM_NBPMEMORY_CLOCK)
				pi->sys_info.nbp_memory_clock[i] =
				le32_to_cpu(igp_info->info_9.ulNbpStateMemclkFreq[i]);
			pi->sys_info.nbp_n_clock[i] =
			le32_to_cpu(igp_info->info_9.ulNbpStateNClkFreq[i]);
		}

		for (i = 0; i < CZ_MAX_DISPLAY_CLOCK_LEVEL; i++)
			pi->sys_info.display_clock[i] =
			le32_to_cpu(igp_info->info_9.sDispClkVoltageMapping[i].ulMaximumSupportedCLK);

		for (i = 0; i < CZ_NUM_NBPSTATES; i++)
			pi->sys_info.nbp_voltage_index[i] =
				le32_to_cpu(igp_info->info_9.usNBPStateVoltage[i]);

		if (le32_to_cpu(igp_info->info_9.ulGPUCapInfo) &
			SYS_INFO_GPUCAPS__ENABEL_DFS_BYPASS)
			pi->caps_enable_dfs_bypass = true;

		pi->sys_info.uma_channel_number =
			igp_info->info_9.ucUMAChannelNumber;

		cz_construct_max_power_limits_table(adev,
			&adev->pm.dpm.dyn_state.max_clock_voltage_on_ac);
	}

	return 0;
}

static void cz_patch_voltage_values(struct amdgpu_device *adev)
{
	int i;
	struct amdgpu_uvd_clock_voltage_dependency_table *uvd_table =
		&adev->pm.dpm.dyn_state.uvd_clock_voltage_dependency_table;
	struct amdgpu_vce_clock_voltage_dependency_table *vce_table =
		&adev->pm.dpm.dyn_state.vce_clock_voltage_dependency_table;
	struct amdgpu_clock_voltage_dependency_table *acp_table =
		&adev->pm.dpm.dyn_state.acp_clock_voltage_dependency_table;

	if (uvd_table->count) {
		for (i = 0; i < uvd_table->count; i++)
			uvd_table->entries[i].v =
				cz_convert_8bit_index_to_voltage(adev,
						uvd_table->entries[i].v);
	}

	if (vce_table->count) {
		for (i = 0; i < vce_table->count; i++)
			vce_table->entries[i].v =
				cz_convert_8bit_index_to_voltage(adev,
						vce_table->entries[i].v);
	}

	if (acp_table->count) {
		for (i = 0; i < acp_table->count; i++)
			acp_table->entries[i].v =
				cz_convert_8bit_index_to_voltage(adev,
						acp_table->entries[i].v);
	}

}

static void cz_construct_boot_state(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);

	pi->boot_pl.sclk = pi->sys_info.bootup_sclk;
	pi->boot_pl.vddc_index = pi->sys_info.bootup_nb_voltage_index;
	pi->boot_pl.ds_divider_index = 0;
	pi->boot_pl.ss_divider_index = 0;
	pi->boot_pl.allow_gnb_slow = 1;
	pi->boot_pl.force_nbp_state = 0;
	pi->boot_pl.display_wm = 0;
	pi->boot_pl.vce_wm = 0;

}

static void cz_patch_boot_state(struct amdgpu_device *adev,
				struct cz_ps *ps)
{
	struct cz_power_info *pi = cz_get_pi(adev);

	ps->num_levels = 1;
	ps->levels[0] = pi->boot_pl;
}

union pplib_clock_info {
	struct _ATOM_PPLIB_EVERGREEN_CLOCK_INFO evergreen;
	struct _ATOM_PPLIB_SUMO_CLOCK_INFO sumo;
	struct _ATOM_PPLIB_CZ_CLOCK_INFO carrizo;
};

static void cz_parse_pplib_clock_info(struct amdgpu_device *adev,
					struct amdgpu_ps *rps, int index,
					union pplib_clock_info *clock_info)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct cz_ps *ps = cz_get_ps(rps);
	struct cz_pl *pl = &ps->levels[index];
	struct amdgpu_clock_voltage_dependency_table *table =
			&adev->pm.dpm.dyn_state.vddc_dependency_on_sclk;

	pl->sclk = table->entries[clock_info->carrizo.index].clk;
	pl->vddc_index = table->entries[clock_info->carrizo.index].v;

	ps->num_levels = index + 1;

	if (pi->caps_sclk_ds) {
		pl->ds_divider_index = 5;
		pl->ss_divider_index = 5;
	}

}

static void cz_parse_pplib_non_clock_info(struct amdgpu_device *adev,
			struct amdgpu_ps *rps,
			struct _ATOM_PPLIB_NONCLOCK_INFO *non_clock_info,
			u8 table_rev)
{
	struct cz_ps *ps = cz_get_ps(rps);

	rps->caps = le32_to_cpu(non_clock_info->ulCapsAndSettings);
	rps->class = le16_to_cpu(non_clock_info->usClassification);
	rps->class2 = le16_to_cpu(non_clock_info->usClassification2);

	if (ATOM_PPLIB_NONCLOCKINFO_VER1 < table_rev) {
		rps->vclk = le32_to_cpu(non_clock_info->ulVCLK);
		rps->dclk = le32_to_cpu(non_clock_info->ulDCLK);
	} else {
		rps->vclk = 0;
		rps->dclk = 0;
	}

	if (rps->class & ATOM_PPLIB_CLASSIFICATION_BOOT) {
		adev->pm.dpm.boot_ps = rps;
		cz_patch_boot_state(adev, ps);
	}
	if (rps->class & ATOM_PPLIB_CLASSIFICATION_UVDSTATE)
		adev->pm.dpm.uvd_ps = rps;

}

union power_info {
	struct _ATOM_PPLIB_POWERPLAYTABLE pplib;
	struct _ATOM_PPLIB_POWERPLAYTABLE2 pplib2;
	struct _ATOM_PPLIB_POWERPLAYTABLE3 pplib3;
	struct _ATOM_PPLIB_POWERPLAYTABLE4 pplib4;
	struct _ATOM_PPLIB_POWERPLAYTABLE5 pplib5;
};

union pplib_power_state {
	struct _ATOM_PPLIB_STATE v1;
	struct _ATOM_PPLIB_STATE_V2 v2;
};

static int cz_parse_power_table(struct amdgpu_device *adev)
{
	struct amdgpu_mode_info *mode_info = &adev->mode_info;
	struct _ATOM_PPLIB_NONCLOCK_INFO *non_clock_info;
	union pplib_power_state *power_state;
	int i, j, k, non_clock_array_index, clock_array_index;
	union pplib_clock_info *clock_info;
	struct _StateArray *state_array;
	struct _ClockInfoArray *clock_info_array;
	struct _NonClockInfoArray *non_clock_info_array;
	union power_info *power_info;
	int index = GetIndexIntoMasterTable(DATA, PowerPlayInfo);
	u16 data_offset;
	u8 frev, crev;
	u8 *power_state_offset;
	struct cz_ps *ps;

	if (!amdgpu_atom_parse_data_header(mode_info->atom_context, index, NULL,
				    &frev, &crev, &data_offset))
		return -EINVAL;
	power_info = (union power_info *)(mode_info->atom_context->bios + data_offset);

	state_array = (struct _StateArray *)
		(mode_info->atom_context->bios + data_offset +
		le16_to_cpu(power_info->pplib.usStateArrayOffset));
	clock_info_array = (struct _ClockInfoArray *)
		(mode_info->atom_context->bios + data_offset +
		le16_to_cpu(power_info->pplib.usClockInfoArrayOffset));
	non_clock_info_array = (struct _NonClockInfoArray *)
		(mode_info->atom_context->bios + data_offset +
		le16_to_cpu(power_info->pplib.usNonClockInfoArrayOffset));

	adev->pm.dpm.ps = kzalloc(sizeof(struct amdgpu_ps) *
					state_array->ucNumEntries, GFP_KERNEL);

	if (!adev->pm.dpm.ps)
		return -ENOMEM;

	power_state_offset = (u8 *)state_array->states;
	adev->pm.dpm.platform_caps =
			le32_to_cpu(power_info->pplib.ulPlatformCaps);
	adev->pm.dpm.backbias_response_time =
			le16_to_cpu(power_info->pplib.usBackbiasTime);
	adev->pm.dpm.voltage_response_time =
			le16_to_cpu(power_info->pplib.usVoltageTime);

	for (i = 0; i < state_array->ucNumEntries; i++) {
		power_state = (union pplib_power_state *)power_state_offset;
		non_clock_array_index = power_state->v2.nonClockInfoIndex;
		non_clock_info = (struct _ATOM_PPLIB_NONCLOCK_INFO *)
			&non_clock_info_array->nonClockInfo[non_clock_array_index];

		ps = kzalloc(sizeof(struct cz_ps), GFP_KERNEL);
		if (ps == NULL) {
			kfree(adev->pm.dpm.ps);
			return -ENOMEM;
		}

		adev->pm.dpm.ps[i].ps_priv = ps;
		k = 0;
		for (j = 0; j < power_state->v2.ucNumDPMLevels; j++) {
			clock_array_index = power_state->v2.clockInfoIndex[j];
			if (clock_array_index >= clock_info_array->ucNumEntries)
				continue;
			if (k >= CZ_MAX_HARDWARE_POWERLEVELS)
				break;
			clock_info = (union pplib_clock_info *)
				&clock_info_array->clockInfo[clock_array_index *
				clock_info_array->ucEntrySize];
			cz_parse_pplib_clock_info(adev, &adev->pm.dpm.ps[i],
				k, clock_info);
			k++;
		}
		cz_parse_pplib_non_clock_info(adev, &adev->pm.dpm.ps[i],
					non_clock_info,
					non_clock_info_array->ucEntrySize);
		power_state_offset += 2 + power_state->v2.ucNumDPMLevels;
	}
	adev->pm.dpm.num_ps = state_array->ucNumEntries;

	return 0;
}

static int cz_process_firmware_header(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	u32 tmp;
	int ret;

	ret = cz_read_smc_sram_dword(adev, SMU8_FIRMWARE_HEADER_LOCATION +
				     offsetof(struct SMU8_Firmware_Header,
				     DpmTable),
				     &tmp, pi->sram_end);

	if (ret == 0)
		pi->dpm_table_start = tmp;

	return ret;
}

static int cz_dpm_init(struct amdgpu_device *adev)
{
	struct cz_power_info *pi;
	int ret, i;

	pi = kzalloc(sizeof(struct cz_power_info), GFP_KERNEL);
	if (NULL == pi)
		return -ENOMEM;

	adev->pm.dpm.priv = pi;

	ret = amdgpu_get_platform_caps(adev);
	if (ret)
		return ret;

	ret = amdgpu_parse_extended_power_table(adev);
	if (ret)
		return ret;

	pi->sram_end = SMC_RAM_END;

	/* set up DPM defaults */
	for (i = 0; i < CZ_MAX_HARDWARE_POWERLEVELS; i++)
		pi->active_target[i] = CZ_AT_DFLT;

	pi->mgcg_cgtt_local0 = 0x0;
	pi->mgcg_cgtt_local1 = 0x0;
	pi->clock_slow_down_step = 25000;
	pi->skip_clock_slow_down = 1;
	pi->enable_nb_ps_policy = 1;
	pi->caps_power_containment = true;
	pi->caps_cac = true;
	pi->didt_enabled = false;
	if (pi->didt_enabled) {
		pi->caps_sq_ramping = true;
		pi->caps_db_ramping = true;
		pi->caps_td_ramping = true;
		pi->caps_tcp_ramping = true;
	}
	pi->caps_sclk_ds = true;
	pi->voting_clients = 0x00c00033;
	pi->auto_thermal_throttling_enabled = true;
	pi->bapm_enabled = false;
	pi->disable_nb_ps3_in_battery = false;
	pi->voltage_drop_threshold = 0;
	pi->caps_sclk_throttle_low_notification = false;
	pi->gfx_pg_threshold = 500;
	pi->caps_fps = true;
	/* uvd */
	pi->caps_uvd_pg = (adev->pg_flags & AMDGPU_PG_SUPPORT_UVD) ? true : false;
	pi->caps_uvd_dpm = true;
	/* vce */
	pi->caps_vce_pg = (adev->pg_flags & AMDGPU_PG_SUPPORT_VCE) ? true : false;
	pi->caps_vce_dpm = true;
	/* acp */
	pi->caps_acp_pg = (adev->pg_flags & AMDGPU_PG_SUPPORT_ACP) ? true : false;
	pi->caps_acp_dpm = true;

	pi->caps_stable_power_state = false;
	pi->nb_dpm_enabled_by_driver = true;
	pi->nb_dpm_enabled = false;
	pi->caps_voltage_island = false;
	/* flags which indicate need to upload pptable */
	pi->need_pptable_upload = true;

	ret = cz_parse_sys_info_table(adev);
	if (ret)
		return ret;

	cz_patch_voltage_values(adev);
	cz_construct_boot_state(adev);

	ret = cz_parse_power_table(adev);
	if (ret)
		return ret;

	ret = cz_process_firmware_header(adev);
	if (ret)
		return ret;

	pi->dpm_enabled = true;

	return 0;
}

static void cz_dpm_fini(struct amdgpu_device *adev)
{
	int i;

	for (i = 0; i < adev->pm.dpm.num_ps; i++)
		kfree(adev->pm.dpm.ps[i].ps_priv);

	kfree(adev->pm.dpm.ps);
	kfree(adev->pm.dpm.priv);
	amdgpu_free_extended_power_table(adev);
}

static void
cz_dpm_debugfs_print_current_performance_level(struct amdgpu_device *adev,
					       struct seq_file *m)
{
	struct amdgpu_clock_voltage_dependency_table *table =
		&adev->pm.dpm.dyn_state.vddc_dependency_on_sclk;
	u32 current_index =
		(RREG32_SMC(ixTARGET_AND_CURRENT_PROFILE_INDEX) &
		TARGET_AND_CURRENT_PROFILE_INDEX__CURR_SCLK_INDEX_MASK) >>
		TARGET_AND_CURRENT_PROFILE_INDEX__CURR_SCLK_INDEX__SHIFT;
	u32 sclk, tmp;
	u16 vddc;

	if (current_index >= NUM_SCLK_LEVELS) {
		seq_printf(m, "invalid dpm profile %d\n", current_index);
	} else {
		sclk = table->entries[current_index].clk;
		tmp = (RREG32_SMC(ixSMU_VOLTAGE_STATUS) &
			SMU_VOLTAGE_STATUS__SMU_VOLTAGE_CURRENT_LEVEL_MASK) >>
			SMU_VOLTAGE_STATUS__SMU_VOLTAGE_CURRENT_LEVEL__SHIFT;
		vddc = cz_convert_8bit_index_to_voltage(adev, (u16)tmp);
		seq_printf(m, "power level %d    sclk: %u vddc: %u\n",
			   current_index, sclk, vddc);
	}
}

static void cz_dpm_print_power_state(struct amdgpu_device *adev,
					struct amdgpu_ps *rps)
{
	int i;
	struct cz_ps *ps = cz_get_ps(rps);

	amdgpu_dpm_print_class_info(rps->class, rps->class2);
	amdgpu_dpm_print_cap_info(rps->caps);

	DRM_INFO("\tuvd    vclk: %d dclk: %d\n", rps->vclk, rps->dclk);
	for (i = 0; i < ps->num_levels; i++) {
		struct cz_pl *pl = &ps->levels[i];

		DRM_INFO("\t\tpower level %d    sclk: %u vddc: %u\n",
		       i, pl->sclk,
		       cz_convert_8bit_index_to_voltage(adev, pl->vddc_index));
	}

	amdgpu_dpm_print_ps_status(adev, rps);
}

static void cz_dpm_set_funcs(struct amdgpu_device *adev);

static int cz_dpm_early_init(struct amdgpu_device *adev)
{
	cz_dpm_set_funcs(adev);

	return 0;
}

static int cz_dpm_sw_init(struct amdgpu_device *adev)
{
	int ret = 0;
	/* fix me to add thermal support TODO */

	/* default to balanced state */
	adev->pm.dpm.state = POWER_STATE_TYPE_BALANCED;
	adev->pm.dpm.user_state = POWER_STATE_TYPE_BALANCED;
	adev->pm.dpm.forced_level = AMDGPU_DPM_FORCED_LEVEL_AUTO;
	adev->pm.default_sclk = adev->clock.default_sclk;
	adev->pm.default_mclk = adev->clock.default_mclk;
	adev->pm.current_sclk = adev->clock.default_sclk;
	adev->pm.current_mclk = adev->clock.default_mclk;
	adev->pm.int_thermal_type = THERMAL_TYPE_NONE;

	if (amdgpu_dpm == 0)
		return 0;

	mutex_lock(&adev->pm.mutex);
	ret = cz_dpm_init(adev);
	if (ret)
		goto dpm_init_failed;

	adev->pm.dpm.current_ps = adev->pm.dpm.requested_ps = adev->pm.dpm.boot_ps;
	if (amdgpu_dpm == 1)
		amdgpu_pm_print_power_states(adev);

	ret = amdgpu_pm_sysfs_init(adev);
	if (ret)
		goto dpm_init_failed;

	mutex_unlock(&adev->pm.mutex);
	DRM_INFO("amdgpu: dpm initialized\n");

	return 0;

dpm_init_failed:
	cz_dpm_fini(adev);
	mutex_unlock(&adev->pm.mutex);
	DRM_ERROR("amdgpu: dpm initialization failed\n");

	return ret;
}

static int cz_dpm_sw_fini(struct amdgpu_device *adev)
{
	mutex_lock(&adev->pm.mutex);
	amdgpu_pm_sysfs_fini(adev);
	cz_dpm_fini(adev);
	mutex_unlock(&adev->pm.mutex);

	return 0;
}

static void cz_reset_ap_mask(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);

	pi->active_process_mask = 0;

}

static int cz_dpm_download_pptable_from_smu(struct amdgpu_device *adev,
							void **table)
{
	int ret = 0;

	ret = cz_smu_download_pptable(adev, table);

	return ret;
}

static int cz_dpm_upload_pptable_to_smu(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct SMU8_Fusion_ClkTable *clock_table;
	struct atom_clock_dividers dividers;
	void *table = NULL;
	uint8_t i = 0;
	int ret = 0;

	struct amdgpu_clock_voltage_dependency_table *vddc_table =
		&adev->pm.dpm.dyn_state.vddc_dependency_on_sclk;
	struct amdgpu_clock_voltage_dependency_table *vddgfx_table =
		&adev->pm.dpm.dyn_state.vddgfx_dependency_on_sclk;
	struct amdgpu_uvd_clock_voltage_dependency_table *uvd_table =
		&adev->pm.dpm.dyn_state.uvd_clock_voltage_dependency_table;
	struct amdgpu_vce_clock_voltage_dependency_table *vce_table =
		&adev->pm.dpm.dyn_state.vce_clock_voltage_dependency_table;
	struct amdgpu_clock_voltage_dependency_table *acp_table =
		&adev->pm.dpm.dyn_state.acp_clock_voltage_dependency_table;

	if (!pi->need_pptable_upload)
		return 0;

	ret = cz_dpm_download_pptable_from_smu(adev, &table);
	if (ret) {
		DRM_ERROR("amdgpu: Failed to get power play table from SMU!\n");
		return -EINVAL;
	}

	clock_table = (struct SMU8_Fusion_ClkTable *)table;
	/* patch clock table */
	if (vddc_table->count > CZ_MAX_HARDWARE_POWERLEVELS ||
			vddgfx_table->count > CZ_MAX_HARDWARE_POWERLEVELS ||
			uvd_table->count > CZ_MAX_HARDWARE_POWERLEVELS ||
			vce_table->count > CZ_MAX_HARDWARE_POWERLEVELS ||
			acp_table->count > CZ_MAX_HARDWARE_POWERLEVELS) {
		DRM_ERROR("amdgpu: Invalid Clock Voltage Dependency Table!\n");
		return -EINVAL;
	}

	for (i = 0; i < CZ_MAX_HARDWARE_POWERLEVELS; i++) {

		/* vddc sclk */
		clock_table->SclkBreakdownTable.ClkLevel[i].GnbVid =
			(i < vddc_table->count) ? (uint8_t)vddc_table->entries[i].v : 0;
		clock_table->SclkBreakdownTable.ClkLevel[i].Frequency =
			(i < vddc_table->count) ? vddc_table->entries[i].clk : 0;
		ret = amdgpu_atombios_get_clock_dividers(adev, COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
				clock_table->SclkBreakdownTable.ClkLevel[i].Frequency,
				false, &dividers);
		if (ret)
			return ret;
		clock_table->SclkBreakdownTable.ClkLevel[i].DfsDid =
						(uint8_t)dividers.post_divider;

		/* vddgfx sclk */
		clock_table->SclkBreakdownTable.ClkLevel[i].GfxVid =
			(i < vddgfx_table->count) ? (uint8_t)vddgfx_table->entries[i].v : 0;

		/* acp breakdown */
		clock_table->AclkBreakdownTable.ClkLevel[i].GfxVid =
			(i < acp_table->count) ? (uint8_t)acp_table->entries[i].v : 0;
		clock_table->AclkBreakdownTable.ClkLevel[i].Frequency =
			(i < acp_table->count) ? acp_table->entries[i].clk : 0;
		ret = amdgpu_atombios_get_clock_dividers(adev, COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
				clock_table->SclkBreakdownTable.ClkLevel[i].Frequency,
				false, &dividers);
		if (ret)
			return ret;
		clock_table->AclkBreakdownTable.ClkLevel[i].DfsDid =
						(uint8_t)dividers.post_divider;

		/* uvd breakdown */
		clock_table->VclkBreakdownTable.ClkLevel[i].GfxVid =
			(i < uvd_table->count) ? (uint8_t)uvd_table->entries[i].v : 0;
		clock_table->VclkBreakdownTable.ClkLevel[i].Frequency =
			(i < uvd_table->count) ? uvd_table->entries[i].vclk : 0;
		ret = amdgpu_atombios_get_clock_dividers(adev, COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
				clock_table->VclkBreakdownTable.ClkLevel[i].Frequency,
				false, &dividers);
		if (ret)
			return ret;
		clock_table->VclkBreakdownTable.ClkLevel[i].DfsDid =
						(uint8_t)dividers.post_divider;

		clock_table->DclkBreakdownTable.ClkLevel[i].GfxVid =
			(i < uvd_table->count) ? (uint8_t)uvd_table->entries[i].v : 0;
		clock_table->DclkBreakdownTable.ClkLevel[i].Frequency =
			(i < uvd_table->count) ? uvd_table->entries[i].dclk : 0;
		ret = amdgpu_atombios_get_clock_dividers(adev, COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
				clock_table->DclkBreakdownTable.ClkLevel[i].Frequency,
				false, &dividers);
		if (ret)
			return ret;
		clock_table->DclkBreakdownTable.ClkLevel[i].DfsDid =
						(uint8_t)dividers.post_divider;

		/* vce breakdown */
		clock_table->EclkBreakdownTable.ClkLevel[i].GfxVid =
			(i < vce_table->count) ? (uint8_t)vce_table->entries[i].v : 0;
		clock_table->EclkBreakdownTable.ClkLevel[i].Frequency =
			(i < vce_table->count) ? vce_table->entries[i].ecclk : 0;
		ret = amdgpu_atombios_get_clock_dividers(adev, COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
				clock_table->EclkBreakdownTable.ClkLevel[i].Frequency,
				false, &dividers);
		if (ret)
			return ret;
		clock_table->EclkBreakdownTable.ClkLevel[i].DfsDid =
						(uint8_t)dividers.post_divider;
	}

	/* its time to upload to SMU */
	ret = cz_smu_upload_pptable(adev);
	if (ret) {
		DRM_ERROR("amdgpu: Failed to put power play table to SMU!\n");
		return ret;
	}

	return 0;
}

static void cz_init_sclk_limit(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_clock_voltage_dependency_table *table =
		&adev->pm.dpm.dyn_state.vddc_dependency_on_sclk;
	uint32_t clock = 0, level;

	if (!table || !table->count) {
		DRM_ERROR("Invalid Voltage Dependency table.\n");
		return;
	}

	pi->sclk_dpm.soft_min_clk = 0;
	pi->sclk_dpm.hard_min_clk = 0;
	cz_send_msg_to_smc(adev, PPSMC_MSG_GetMaxSclkLevel);
	level = cz_get_argument(adev);
	if (level < table->count)
		clock = table->entries[level].clk;
	else {
		DRM_ERROR("Invalid SLCK Voltage Dependency table entry.\n");
		clock = table->entries[table->count - 1].clk;
	}

	pi->sclk_dpm.soft_max_clk = clock;
	pi->sclk_dpm.hard_max_clk = clock;

}

static void cz_init_uvd_limit(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_uvd_clock_voltage_dependency_table *table =
		&adev->pm.dpm.dyn_state.uvd_clock_voltage_dependency_table;
	uint32_t clock = 0, level;

	if (!table || !table->count) {
		DRM_ERROR("Invalid Voltage Dependency table.\n");
		return;
	}

	pi->uvd_dpm.soft_min_clk = 0;
	pi->uvd_dpm.hard_min_clk = 0;
	cz_send_msg_to_smc(adev, PPSMC_MSG_GetMaxUvdLevel);
	level = cz_get_argument(adev);
	if (level < table->count)
		clock = table->entries[level].vclk;
	else {
		DRM_ERROR("Invalid UVD Voltage Dependency table entry.\n");
		clock = table->entries[table->count - 1].vclk;
	}

	pi->uvd_dpm.soft_max_clk = clock;
	pi->uvd_dpm.hard_max_clk = clock;

}

static void cz_init_vce_limit(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_vce_clock_voltage_dependency_table *table =
		&adev->pm.dpm.dyn_state.vce_clock_voltage_dependency_table;
	uint32_t clock = 0, level;

	if (!table || !table->count) {
		DRM_ERROR("Invalid Voltage Dependency table.\n");
		return;
	}

	pi->vce_dpm.soft_min_clk = 0;
	pi->vce_dpm.hard_min_clk = 0;
	cz_send_msg_to_smc(adev, PPSMC_MSG_GetMaxEclkLevel);
	level = cz_get_argument(adev);
	if (level < table->count)
		clock = table->entries[level].evclk;
	else {
		/* future BIOS would fix this error */
		DRM_ERROR("Invalid VCE Voltage Dependency table entry.\n");
		clock = table->entries[table->count - 1].evclk;
	}

	pi->vce_dpm.soft_max_clk = clock;
	pi->vce_dpm.hard_max_clk = clock;

}

static void cz_init_acp_limit(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_clock_voltage_dependency_table *table =
		&adev->pm.dpm.dyn_state.acp_clock_voltage_dependency_table;
	uint32_t clock = 0, level;

	if (!table || !table->count) {
		DRM_ERROR("Invalid Voltage Dependency table.\n");
		return;
	}

	pi->acp_dpm.soft_min_clk = 0;
	pi->acp_dpm.hard_min_clk = 0;
	cz_send_msg_to_smc(adev, PPSMC_MSG_GetMaxAclkLevel);
	level = cz_get_argument(adev);
	if (level < table->count)
		clock = table->entries[level].clk;
	else {
		DRM_ERROR("Invalid ACP Voltage Dependency table entry.\n");
		clock = table->entries[table->count - 1].clk;
	}

	pi->acp_dpm.soft_max_clk = clock;
	pi->acp_dpm.hard_max_clk = clock;

}

static void cz_init_pg_state(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);

	pi->uvd_power_gated = false;
	pi->vce_power_gated = false;
	pi->acp_power_gated = false;

}

static void cz_init_sclk_threshold(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);

	pi->low_sclk_interrupt_threshold = 0;

}

static void cz_dpm_setup_asic(struct amdgpu_device *adev)
{
	cz_reset_ap_mask(adev);
	cz_dpm_upload_pptable_to_smu(adev);
	cz_init_sclk_limit(adev);
	cz_init_uvd_limit(adev);
	cz_init_vce_limit(adev);
	cz_init_acp_limit(adev);
	cz_init_pg_state(adev);
	cz_init_sclk_threshold(adev);

}

static bool cz_check_smu_feature(struct amdgpu_device *adev,
				uint32_t feature)
{
	uint32_t smu_feature = 0;
	int ret;

	ret = cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_GetFeatureStatus, 0);
	if (ret) {
		DRM_ERROR("Failed to get SMU features from SMC.\n");
		return false;
	} else {
		smu_feature = cz_get_argument(adev);
		if (feature & smu_feature)
			return true;
	}

	return false;
}

static bool cz_check_for_dpm_enabled(struct amdgpu_device *adev)
{
	if (cz_check_smu_feature(adev,
				SMU_EnabledFeatureScoreboard_SclkDpmOn))
		return true;

	return false;
}

static void cz_program_voting_clients(struct amdgpu_device *adev)
{
	WREG32_SMC(ixCG_FREQ_TRAN_VOTING_0, PPCZ_VOTINGRIGHTSCLIENTS_DFLT0);
}

static void cz_clear_voting_clients(struct amdgpu_device *adev)
{
	WREG32_SMC(ixCG_FREQ_TRAN_VOTING_0, 0);
}

static int cz_start_dpm(struct amdgpu_device *adev)
{
	int ret = 0;

	if (amdgpu_dpm) {
		ret = cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_EnableAllSmuFeatures, SCLK_DPM_MASK);
		if (ret) {
			DRM_ERROR("SMU feature: SCLK_DPM enable failed\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int cz_stop_dpm(struct amdgpu_device *adev)
{
	int ret = 0;

	if (amdgpu_dpm && adev->pm.dpm_enabled) {
		ret = cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_DisableAllSmuFeatures, SCLK_DPM_MASK);
		if (ret) {
			DRM_ERROR("SMU feature: SCLK_DPM disable failed\n");
			return -EINVAL;
		}
	}

	return 0;
}

static uint32_t cz_get_sclk_level(struct amdgpu_device *adev,
				uint32_t clock, uint16_t msg)
{
	int i = 0;
	struct amdgpu_clock_voltage_dependency_table *table =
		&adev->pm.dpm.dyn_state.vddc_dependency_on_sclk;

	switch (msg) {
	case PPSMC_MSG_SetSclkSoftMin:
	case PPSMC_MSG_SetSclkHardMin:
		for (i = 0; i < table->count; i++)
			if (clock <= table->entries[i].clk)
				break;
		if (i == table->count)
			i = table->count - 1;
		break;
	case PPSMC_MSG_SetSclkSoftMax:
	case PPSMC_MSG_SetSclkHardMax:
		for (i = table->count - 1; i >= 0; i--)
			if (clock >= table->entries[i].clk)
				break;
		if (i < 0)
			i = 0;
		break;
	default:
		break;
	}

	return i;
}

static int cz_program_bootup_state(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	uint32_t soft_min_clk = 0;
	uint32_t soft_max_clk = 0;
	int ret = 0;

	pi->sclk_dpm.soft_min_clk = pi->sys_info.bootup_sclk;
	pi->sclk_dpm.soft_max_clk = pi->sys_info.bootup_sclk;

	soft_min_clk = cz_get_sclk_level(adev,
				pi->sclk_dpm.soft_min_clk,
				PPSMC_MSG_SetSclkSoftMin);
	soft_max_clk = cz_get_sclk_level(adev,
				pi->sclk_dpm.soft_max_clk,
				PPSMC_MSG_SetSclkSoftMax);

	ret = cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_SetSclkSoftMin, soft_min_clk);
	if (ret)
		return -EINVAL;

	ret = cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_SetSclkSoftMax, soft_max_clk);
	if (ret)
		return -EINVAL;

	return 0;
}

/* TODO */
static int cz_disable_cgpg(struct amdgpu_device *adev)
{
	return 0;
}

/* TODO */
static int cz_enable_cgpg(struct amdgpu_device *adev)
{
	return 0;
}

/* TODO */
static int cz_program_pt_config_registers(struct amdgpu_device *adev)
{
	return 0;
}

static void cz_do_enable_didt(struct amdgpu_device *adev, bool enable)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	uint32_t reg = 0;

	if (pi->caps_sq_ramping) {
		reg = RREG32_DIDT(ixDIDT_SQ_CTRL0);
		if (enable)
			reg = REG_SET_FIELD(reg, DIDT_SQ_CTRL0, DIDT_CTRL_EN, 1);
		else
			reg = REG_SET_FIELD(reg, DIDT_SQ_CTRL0, DIDT_CTRL_EN, 0);
		WREG32_DIDT(ixDIDT_SQ_CTRL0, reg);
	}
	if (pi->caps_db_ramping) {
		reg = RREG32_DIDT(ixDIDT_DB_CTRL0);
		if (enable)
			reg = REG_SET_FIELD(reg, DIDT_DB_CTRL0, DIDT_CTRL_EN, 1);
		else
			reg = REG_SET_FIELD(reg, DIDT_DB_CTRL0, DIDT_CTRL_EN, 0);
		WREG32_DIDT(ixDIDT_DB_CTRL0, reg);
	}
	if (pi->caps_td_ramping) {
		reg = RREG32_DIDT(ixDIDT_TD_CTRL0);
		if (enable)
			reg = REG_SET_FIELD(reg, DIDT_TD_CTRL0, DIDT_CTRL_EN, 1);
		else
			reg = REG_SET_FIELD(reg, DIDT_TD_CTRL0, DIDT_CTRL_EN, 0);
		WREG32_DIDT(ixDIDT_TD_CTRL0, reg);
	}
	if (pi->caps_tcp_ramping) {
		reg = RREG32_DIDT(ixDIDT_TCP_CTRL0);
		if (enable)
			reg = REG_SET_FIELD(reg, DIDT_SQ_CTRL0, DIDT_CTRL_EN, 1);
		else
			reg = REG_SET_FIELD(reg, DIDT_SQ_CTRL0, DIDT_CTRL_EN, 0);
		WREG32_DIDT(ixDIDT_TCP_CTRL0, reg);
	}

}

static int cz_enable_didt(struct amdgpu_device *adev, bool enable)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	int ret;

	if (pi->caps_sq_ramping || pi->caps_db_ramping ||
			pi->caps_td_ramping || pi->caps_tcp_ramping) {
		if (adev->gfx.gfx_current_status != AMDGPU_GFX_SAFE_MODE) {
			ret = cz_disable_cgpg(adev);
			if (ret) {
				DRM_ERROR("Pre Di/Dt disable cg/pg failed\n");
				return -EINVAL;
			}
			adev->gfx.gfx_current_status = AMDGPU_GFX_SAFE_MODE;
		}

		ret = cz_program_pt_config_registers(adev);
		if (ret) {
			DRM_ERROR("Di/Dt config failed\n");
			return -EINVAL;
		}
		cz_do_enable_didt(adev, enable);

		if (adev->gfx.gfx_current_status == AMDGPU_GFX_SAFE_MODE) {
			ret = cz_enable_cgpg(adev);
			if (ret) {
				DRM_ERROR("Post Di/Dt enable cg/pg failed\n");
				return -EINVAL;
			}
			adev->gfx.gfx_current_status = AMDGPU_GFX_NORMAL_MODE;
		}
	}

	return 0;
}

/* TODO */
static void cz_reset_acp_boot_level(struct amdgpu_device *adev)
{
}

static void cz_update_current_ps(struct amdgpu_device *adev,
					struct amdgpu_ps *rps)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct cz_ps *ps = cz_get_ps(rps);

	pi->current_ps = *ps;
	pi->current_rps = *rps;
	pi->current_rps.ps_priv = ps;

}

static void cz_update_requested_ps(struct amdgpu_device *adev,
					struct amdgpu_ps *rps)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct cz_ps *ps = cz_get_ps(rps);

	pi->requested_ps = *ps;
	pi->requested_rps = *rps;
	pi->requested_rps.ps_priv = ps;

}

/* PP arbiter support needed TODO */
static void cz_apply_state_adjust_rules(struct amdgpu_device *adev,
					struct amdgpu_ps *new_rps,
					struct amdgpu_ps *old_rps)
{
	struct cz_ps *ps = cz_get_ps(new_rps);
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_clock_and_voltage_limits *limits =
		&adev->pm.dpm.dyn_state.max_clock_voltage_on_ac;
	/* 10kHz memory clock */
	uint32_t mclk = 0;

	ps->force_high = false;
	ps->need_dfs_bypass = true;
	pi->video_start = new_rps->dclk || new_rps->vclk ||
				new_rps->evclk || new_rps->ecclk;

	if ((new_rps->class & ATOM_PPLIB_CLASSIFICATION_UI_MASK) ==
			ATOM_PPLIB_CLASSIFICATION_UI_BATTERY)
		pi->battery_state = true;
	else
		pi->battery_state = false;

	if (pi->caps_stable_power_state)
		mclk = limits->mclk;

	if (mclk > pi->sys_info.nbp_memory_clock[CZ_NUM_NBPMEMORY_CLOCK - 1])
		ps->force_high = true;

}

static int cz_dpm_enable(struct amdgpu_device *adev)
{
	int ret = 0;

	/* renable will hang up SMU, so check first */
	if (cz_check_for_dpm_enabled(adev))
		return -EINVAL;

	cz_program_voting_clients(adev);

	ret = cz_start_dpm(adev);
	if (ret) {
		DRM_ERROR("Carrizo DPM enable failed\n");
		return -EINVAL;
	}

	ret = cz_program_bootup_state(adev);
	if (ret) {
		DRM_ERROR("Carrizo bootup state program failed\n");
		return -EINVAL;
	}

	ret = cz_enable_didt(adev, true);
	if (ret) {
		DRM_ERROR("Carrizo enable di/dt failed\n");
		return -EINVAL;
	}

	cz_reset_acp_boot_level(adev);

	cz_update_current_ps(adev, adev->pm.dpm.boot_ps);

	return 0;
}

static int cz_dpm_hw_init(struct amdgpu_device *adev)
{
	int ret;

	if (!amdgpu_dpm)
		return 0;

	mutex_lock(&adev->pm.mutex);

	/* init smc in dpm hw init */
	ret = cz_smu_init(adev);
	if (ret) {
		DRM_ERROR("amdgpu: smc initialization failed\n");
		mutex_unlock(&adev->pm.mutex);
		return ret;
	}

	/* do the actual fw loading */
	ret = cz_smu_start(adev);
	if (ret) {
		DRM_ERROR("amdgpu: smc start failed\n");
		mutex_unlock(&adev->pm.mutex);
		return ret;
	}

	/* cz dpm setup asic */
	cz_dpm_setup_asic(adev);

	/* cz dpm enable */
	ret = cz_dpm_enable(adev);
	if (ret)
		adev->pm.dpm_enabled = false;
	else
		adev->pm.dpm_enabled = true;

	mutex_unlock(&adev->pm.mutex);

	return 0;
}

static int cz_dpm_disable(struct amdgpu_device *adev)
{
	int ret = 0;

	if (!cz_check_for_dpm_enabled(adev))
		return -EINVAL;

	ret = cz_enable_didt(adev, false);
	if (ret) {
		DRM_ERROR("Carrizo disable di/dt failed\n");
		return -EINVAL;
	}

	cz_clear_voting_clients(adev);
	cz_stop_dpm(adev);
	cz_update_current_ps(adev, adev->pm.dpm.boot_ps);

	return 0;
}

static int cz_dpm_hw_fini(struct amdgpu_device *adev)
{
	int ret = 0;

	mutex_lock(&adev->pm.mutex);

	cz_smu_fini(adev);

	if (adev->pm.dpm_enabled) {
		ret = cz_dpm_disable(adev);
		if (ret)
			return -EINVAL;

		adev->pm.dpm.current_ps =
			adev->pm.dpm.requested_ps =
			adev->pm.dpm.boot_ps;
	}

	adev->pm.dpm_enabled = false;

	mutex_unlock(&adev->pm.mutex);

	return 0;
}

static int cz_dpm_suspend(struct amdgpu_device *adev)
{
	int ret = 0;

	if (adev->pm.dpm_enabled) {
		mutex_lock(&adev->pm.mutex);

		ret = cz_dpm_disable(adev);
		if (ret)
			return -EINVAL;

		adev->pm.dpm.current_ps =
			adev->pm.dpm.requested_ps =
			adev->pm.dpm.boot_ps;

		mutex_unlock(&adev->pm.mutex);
	}

	return 0;
}

static int cz_dpm_resume(struct amdgpu_device *adev)
{
	int ret = 0;

	mutex_lock(&adev->pm.mutex);
	ret = cz_smu_init(adev);
	if (ret) {
		DRM_ERROR("amdgpu: smc resume failed\n");
		mutex_unlock(&adev->pm.mutex);
		return ret;
	}

	/* do the actual fw loading */
	ret = cz_smu_start(adev);
	if (ret) {
		DRM_ERROR("amdgpu: smc start failed\n");
		mutex_unlock(&adev->pm.mutex);
		return ret;
	}

	/* cz dpm setup asic */
	cz_dpm_setup_asic(adev);

	/* cz dpm enable */
	ret = cz_dpm_enable(adev);
	if (ret)
		adev->pm.dpm_enabled = false;
	else
		adev->pm.dpm_enabled = true;

	mutex_unlock(&adev->pm.mutex);
	/* upon resume, re-compute the clocks */
	if (adev->pm.dpm_enabled)
		amdgpu_pm_compute_clocks(adev);

	return 0;
}

static int cz_dpm_set_clockgating_state(struct amdgpu_device *adev,
					enum amdgpu_clockgating_state state)
{
	return 0;
}

static int cz_dpm_set_powergating_state(struct amdgpu_device *adev,
					enum amdgpu_powergating_state state)
{
	return 0;
}

/* borrowed from KV, need future unify */
static int cz_dpm_get_temperature(struct amdgpu_device *adev)
{
	int actual_temp = 0;
	uint32_t temp = RREG32_SMC(0xC0300E0C);

	if (temp)
		actual_temp = 1000 * ((temp / 8) - 49);

	return actual_temp;
}

static int cz_dpm_pre_set_power_state(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_ps requested_ps = *adev->pm.dpm.requested_ps;
	struct amdgpu_ps *new_ps = &requested_ps;

	cz_update_requested_ps(adev, new_ps);
	cz_apply_state_adjust_rules(adev, &pi->requested_rps,
					&pi->current_rps);

	return 0;
}

static int cz_dpm_update_sclk_limit(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_clock_and_voltage_limits *limits =
		&adev->pm.dpm.dyn_state.max_clock_voltage_on_ac;
	uint32_t clock, stable_ps_clock = 0;

	clock = pi->sclk_dpm.soft_min_clk;

	if (pi->caps_stable_power_state) {
		stable_ps_clock = limits->sclk * 75 / 100;
		if (clock < stable_ps_clock)
			clock = stable_ps_clock;
	}

	if (clock != pi->sclk_dpm.soft_min_clk) {
		pi->sclk_dpm.soft_min_clk = clock;
		cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_SetSclkSoftMin,
				cz_get_sclk_level(adev, clock,
					PPSMC_MSG_SetSclkSoftMin));
	}

	if (pi->caps_stable_power_state &&
			pi->sclk_dpm.soft_max_clk != clock) {
		pi->sclk_dpm.soft_max_clk = clock;
		cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_SetSclkSoftMax,
				cz_get_sclk_level(adev, clock,
					PPSMC_MSG_SetSclkSoftMax));
	} else {
		cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_SetSclkSoftMax,
				cz_get_sclk_level(adev,
					pi->sclk_dpm.soft_max_clk,
					PPSMC_MSG_SetSclkSoftMax));
	}

	return 0;
}

static int cz_dpm_set_deep_sleep_sclk_threshold(struct amdgpu_device *adev)
{
	int ret = 0;
	struct cz_power_info *pi = cz_get_pi(adev);

	if (pi->caps_sclk_ds) {
		cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_SetMinDeepSleepSclk,
				CZ_MIN_DEEP_SLEEP_SCLK);
	}

	return ret;
}

/* ?? without dal support, is this still needed in setpowerstate list*/
static int cz_dpm_set_watermark_threshold(struct amdgpu_device *adev)
{
	int ret = 0;
	struct cz_power_info *pi = cz_get_pi(adev);

	cz_send_msg_to_smc_with_parameter(adev,
			PPSMC_MSG_SetWatermarkFrequency,
			pi->sclk_dpm.soft_max_clk);

	return ret;
}

static int cz_dpm_enable_nbdpm(struct amdgpu_device *adev)
{
	int ret = 0;
	struct cz_power_info *pi = cz_get_pi(adev);

	/* also depend on dal NBPStateDisableRequired */
	if (pi->nb_dpm_enabled_by_driver && !pi->nb_dpm_enabled) {
		ret = cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_EnableAllSmuFeatures,
				NB_DPM_MASK);
		if (ret) {
			DRM_ERROR("amdgpu: nb dpm enable failed\n");
			return ret;
		}
		pi->nb_dpm_enabled = true;
	}

	return ret;
}

static void cz_dpm_nbdpm_lm_pstate_enable(struct amdgpu_device *adev,
							bool enable)
{
	if (enable)
		cz_send_msg_to_smc(adev, PPSMC_MSG_EnableLowMemoryPstate);
	else
		cz_send_msg_to_smc(adev, PPSMC_MSG_DisableLowMemoryPstate);

}

static int cz_dpm_update_low_memory_pstate(struct amdgpu_device *adev)
{
	int ret = 0;
	struct cz_power_info *pi = cz_get_pi(adev);
	struct cz_ps *ps = &pi->requested_ps;

	if (pi->sys_info.nb_dpm_enable) {
		if (ps->force_high)
			cz_dpm_nbdpm_lm_pstate_enable(adev, true);
		else
			cz_dpm_nbdpm_lm_pstate_enable(adev, false);
	}

	return ret;
}

/* with dpm enabled */
static int cz_dpm_set_power_state(struct amdgpu_device *adev)
{
	int ret = 0;

	cz_dpm_update_sclk_limit(adev);
	cz_dpm_set_deep_sleep_sclk_threshold(adev);
	cz_dpm_set_watermark_threshold(adev);
	cz_dpm_enable_nbdpm(adev);
	cz_dpm_update_low_memory_pstate(adev);

	return ret;
}

static void cz_dpm_post_set_power_state(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_ps *ps = &pi->requested_rps;

	cz_update_current_ps(adev, ps);

}

static int cz_dpm_force_highest(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	int ret = 0;

	if (pi->sclk_dpm.soft_min_clk != pi->sclk_dpm.soft_max_clk) {
		pi->sclk_dpm.soft_min_clk =
			pi->sclk_dpm.soft_max_clk;
		ret = cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_SetSclkSoftMin,
				cz_get_sclk_level(adev,
					pi->sclk_dpm.soft_min_clk,
					PPSMC_MSG_SetSclkSoftMin));
		if (ret)
			return ret;
	}

	return ret;
}

static int cz_dpm_force_lowest(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	int ret = 0;

	if (pi->sclk_dpm.soft_max_clk != pi->sclk_dpm.soft_min_clk) {
		pi->sclk_dpm.soft_max_clk = pi->sclk_dpm.soft_min_clk;
		ret = cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_SetSclkSoftMax,
				cz_get_sclk_level(adev,
					pi->sclk_dpm.soft_max_clk,
					PPSMC_MSG_SetSclkSoftMax));
		if (ret)
			return ret;
	}

	return ret;
}

static uint32_t cz_dpm_get_max_sclk_level(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);

	if (!pi->max_sclk_level) {
		cz_send_msg_to_smc(adev, PPSMC_MSG_GetMaxSclkLevel);
		pi->max_sclk_level = cz_get_argument(adev) + 1;
	}

	if (pi->max_sclk_level > CZ_MAX_HARDWARE_POWERLEVELS) {
		DRM_ERROR("Invalid max sclk level!\n");
		return -EINVAL;
	}

	return pi->max_sclk_level;
}

static int cz_dpm_unforce_dpm_levels(struct amdgpu_device *adev)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct amdgpu_clock_voltage_dependency_table *dep_table =
		&adev->pm.dpm.dyn_state.vddc_dependency_on_sclk;
	uint32_t level = 0;
	int ret = 0;

	pi->sclk_dpm.soft_min_clk = dep_table->entries[0].clk;
	level = cz_dpm_get_max_sclk_level(adev) - 1;
	if (level < dep_table->count)
		pi->sclk_dpm.soft_max_clk = dep_table->entries[level].clk;
	else
		pi->sclk_dpm.soft_max_clk =
			dep_table->entries[dep_table->count - 1].clk;

	/* get min/max sclk soft value
	 * notify SMU to execute */
	ret = cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_SetSclkSoftMin,
				cz_get_sclk_level(adev,
					pi->sclk_dpm.soft_min_clk,
					PPSMC_MSG_SetSclkSoftMin));
	if (ret)
		return ret;

	ret = cz_send_msg_to_smc_with_parameter(adev,
				PPSMC_MSG_SetSclkSoftMax,
				cz_get_sclk_level(adev,
					pi->sclk_dpm.soft_max_clk,
					PPSMC_MSG_SetSclkSoftMax));
	if (ret)
		return ret;

	DRM_INFO("DPM unforce state min=%d, max=%d.\n",
			pi->sclk_dpm.soft_min_clk,
			pi->sclk_dpm.soft_max_clk);

	return 0;
}

static int cz_dpm_force_dpm_level(struct amdgpu_device *adev,
				enum amdgpu_dpm_forced_level level)
{
	int ret = 0;

	switch (level) {
	case AMDGPU_DPM_FORCED_LEVEL_HIGH:
		ret = cz_dpm_force_highest(adev);
		if (ret)
			return ret;
		break;
	case AMDGPU_DPM_FORCED_LEVEL_LOW:
		ret = cz_dpm_force_lowest(adev);
		if (ret)
			return ret;
		break;
	case AMDGPU_DPM_FORCED_LEVEL_AUTO:
		ret = cz_dpm_unforce_dpm_levels(adev);
		if (ret)
			return ret;
		break;
	default:
		break;
	}

	return ret;
}

/* fix me, display configuration change lists here
 * mostly dal related*/
static void cz_dpm_display_configuration_changed(struct amdgpu_device *adev)
{
}

static uint32_t cz_dpm_get_sclk(struct amdgpu_device *adev, bool low)
{
	struct cz_power_info *pi = cz_get_pi(adev);
	struct cz_ps *requested_state = cz_get_ps(&pi->requested_rps);

	if (low)
		return requested_state->levels[0].sclk;
	else
		return requested_state->levels[requested_state->num_levels - 1].sclk;

}

static uint32_t cz_dpm_get_mclk(struct amdgpu_device *adev, bool low)
{
	struct cz_power_info *pi = cz_get_pi(adev);

	return pi->sys_info.bootup_uma_clk;
}

const struct amdgpu_ip_funcs cz_dpm_ip_funcs = {
	.early_init = cz_dpm_early_init,
	.late_init = NULL,
	.sw_init = cz_dpm_sw_init,
	.sw_fini = cz_dpm_sw_fini,
	.hw_init = cz_dpm_hw_init,
	.hw_fini = cz_dpm_hw_fini,
	.suspend = cz_dpm_suspend,
	.resume = cz_dpm_resume,
	.is_idle = NULL,
	.wait_for_idle = NULL,
	.soft_reset = NULL,
	.print_status = NULL,
	.set_clockgating_state = cz_dpm_set_clockgating_state,
	.set_powergating_state = cz_dpm_set_powergating_state,
};

static const struct amdgpu_dpm_funcs cz_dpm_funcs = {
	.get_temperature = cz_dpm_get_temperature,
	.pre_set_power_state = cz_dpm_pre_set_power_state,
	.set_power_state = cz_dpm_set_power_state,
	.post_set_power_state = cz_dpm_post_set_power_state,
	.display_configuration_changed = cz_dpm_display_configuration_changed,
	.get_sclk = cz_dpm_get_sclk,
	.get_mclk = cz_dpm_get_mclk,
	.print_power_state = cz_dpm_print_power_state,
	.debugfs_print_current_performance_level =
				cz_dpm_debugfs_print_current_performance_level,
	.force_performance_level = cz_dpm_force_dpm_level,
	.vblank_too_short = NULL,
	.powergate_uvd = NULL,
};

static void cz_dpm_set_funcs(struct amdgpu_device *adev)
{
	if (NULL == adev->pm.funcs)
		adev->pm.funcs = &cz_dpm_funcs;
}
