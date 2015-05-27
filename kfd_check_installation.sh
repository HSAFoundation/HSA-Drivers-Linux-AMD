#!/bin/bash
# Written by Oded Gabbay
# oded.gabbay@amd.com
# openSUSE support and detection of built-in modules by Joerg Roedel <jroedel@suse.de>

# Add /sbin and /usr/sbin to path in case it is not already and lspci is there
PATH=$PATH:/sbin:/usr/sbin

usage()
{
	echo -e "\nusage: $1 [options]\n"
	echo -e "options:\n"
	echo -e "  -h,  --help                  Prints this help"
}

__scriptname="kfd_check_installation.sh"
__pass_flag="YES"
kv_supported_types="1304 1305 1306 1307 1309 130a 130b 130c 130d 130e 130f 1310 1311 1312 1313 1315 1316 1317 1318 131b 131c 131d"
cz_supported_types="9874"
kfd_supported_perms="666 777"

# parameter while-loop
while [[ "$1" != "" ]];
do
	case $1 in
	-h  | --help )
		usage $__scriptname
		exit 0
		;;
	*)
		echo "The parameter $1 is not allowed"
		usage $__scriptname
		exit 1 # error
		;;
	esac
	shift
done

kv_exists=$(lspci -nn | grep -c Kaveri)

if [[ $kv_exists != "0" ]]; then
	kv_exists_result="Yes"
	kv_type=$(lspci -nn | grep VGA | awk -F':' '{print $4}' | awk -F']' '{print $1}')
	if [[ ! -n "$kv_type" ]]; then
		kv_type_result="NO"
		__pass_flag="NO"
	elif [[ $kv_supported_types =~ $kv_type ]]; then
		kv_type_result="Yes"
	else
		kv_type_result="NO"
		__pass_flag="NO"
	fi
else
	kv_exists_result="NO"
	kv_type_result="N/A"

	cz_exists_result="NO"
	for i in $cz_supported_types; do
		cz_exists=$(lspci -nn | grep -c "1002:$i")
		if [[ $cz_exists != "0" ]]; then
			cz_exists_result="Yes"
			cz_type_result="Yes"
		fi
	done

	if [[ $cz_exists_result == "NO" ]]; then
		cz_type_result="N/A"
		__pass_flag="NO"
	fi
fi

radeon_exists=$(grep -c -w radeon_pci_probe /proc/kallsyms)
amdgpu_exists=$(grep -c -w amdgpu_pci_probe /proc/kallsyms)
kfd_exists=$(grep -c -w kgd2kfd_init /proc/kallsyms)
iommu_exists=$(grep -c -w amd_iommu_bind_pasid /proc/kallsyms)

if [[ $kv_exists_result == "Yes" ]]; then
	if [[ $radeon_exists == "1" ]]; then
		radeon_exists_result="Yes"
		radeon_blacklisted="0"
	else
		radeon_exists_result="NO"
		radeon_blacklisted=$(grep blacklist /etc/modprobe.d/* | grep -c -w radeon)
		__pass_flag="NO"
	fi
fi

if [[ $cz_exists_result == "Yes" ]]; then
	if [[ $amdgpu_exists == "1" ]]; then
		amdgpu_exists_result="Yes"
		amdgpu_blacklisted="0"
	else
		amdgpu_exists_result="NO"
		amdgpu_blacklisted=$(grep blacklist /etc/modprobe.d/* | grep -c -w amdgpu)
		__pass_flag="NO"
	fi
fi

if [[ $kfd_exists == "1" ]]; then
	kfd_exists_result="Yes"
else
	kfd_exists_result="NO"
	__pass_flag="NO"
fi

if [[ $iommu_exists == "1" ]]; then
	iommu_exists_result="Yes"
else
	iommu_exists_result="NO"
	__pass_flag="NO"
fi

if [[ -e /dev/kfd ]]; then
	kfd_dev_exists_result="Yes"
	kfd_perms=$(stat -c %a /dev/kfd)
	if [[ $kfd_supported_perms == *$kfd_perms* ]]; then
		kfd_perms_result="Yes"
	else
		kfd_perms_result="NO"
		__pass_flag="NO"
	fi
else
	kfd_dev_exists_result="NO"
	kfd_perms_result="NO"
	__pass_flag="NO"
fi

if [[ -e /sys/devices/virtual/kfd/kfd/topology/nodes/0/gpu_id ]]; then
	gpu_id_result="Yes"
	gpu_id=$(cat /sys/devices/virtual/kfd/kfd/topology/nodes/0/gpu_id)
	if [[ $gpu_id == "0" ]]; then
		gpu_id_result="NO"
		__pass_flag="NO"
	fi
else
	gpu_id_result="NO"
	__pass_flag="NO"
fi

# Print results

echo -e ""
if [[ $kv_exists_result == "Yes" ]]; then
	echo -e "Kaveri detected:............................$kv_exists_result"
	echo -e "Kaveri type supported:......................$kv_type_result"
	echo -e "radeon module is loaded:....................$radeon_exists_result"
	if [[ ! $radeon_blacklisted == "0" ]]; then
		echo -e "Radeon module is blacklisted!!!"
	fi
else
	echo -e "Carrizo detected:...........................$cz_exists_result"
	if [[ $cz_exists_result == "Yes" ]]; then
		echo -e "Carrizo type supported:.....................$cz_type_result"
		echo -e "amdgpu module is loaded:....................$amdgpu_exists_result"
		if [[ ! $amdgpu_blacklisted == "0" ]]; then
			echo -e "amdgpu module is blacklisted!!!"
		fi
	else
		echo -e "Kaveri detected:............................$kv_exists_result"
	fi
fi
echo -e "amdkfd module is loaded:....................$kfd_exists_result"
echo -e "AMD IOMMU V2 module is loaded:..............$iommu_exists_result"
echo -e "KFD device exists:..........................$kfd_dev_exists_result"
echo -e "KFD device has correct permissions:.........$kfd_perms_result"
echo -e "Valid GPU ID is detected:...................$gpu_id_result"
echo -e ""
echo -e "Can run HSA.................................$__pass_flag"
