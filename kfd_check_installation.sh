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

kv_exists=`lspci -nn | grep Kaveri | wc -l`

if [[ $kv_exists != "0" ]]; then
	kv_exists_result="Yes"
	kv_type=`lspci -nn | grep Kaveri | awk '{print $12}' | awk -F':' '{print $2}' | awk -F']' '{print $1}'`
	if [[ $kv_supported_types == *$kv_type* ]]; then
		kv_type_result="Yes"
	else
		kv_type_result="NO"
		__pass_flag="NO"
	fi
else
	kv_exists_result="NO"
	kv_type_result="N/A"
	__pass_flag="NO"
fi

radeon_exists=`grep -w radeon_pci_probe /proc/kallsyms | wc -l`
kfd_exists=`grep -w kgd2kfd_init /proc/kallsyms | wc -l`
iommu_exists=`grep -w amd_iommu_bind_pasid /proc/kallsyms | wc -l`

if [[ $radeon_exists == "1" ]]; then
	radeon_exists_result="Yes"
	radeon_blacklisted="0"
else
	radeon_exists_result="NO"
	__pass_flag="NO"
	radeon_blacklisted=`grep blacklist /etc/modprobe.d/* | grep -w radeon | wc -l`
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
	kfd_perms=`stat -c %a /dev/kfd`
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
	gpu_id=`cat /sys/devices/virtual/kfd/kfd/topology/nodes/0/gpu_id`
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
echo -e "Kaveri detected:............................$kv_exists_result"
echo -e "Kaveri type supported:......................$kv_type_result"
echo -e "Radeon module is loaded:....................$radeon_exists_result"
if [[ ! $radeon_blacklisted == "0" ]]; then
	echo -e "Radeon module is blacklisted!!!"
fi
echo -e "KFD module is loaded:.......................$kfd_exists_result"
echo -e "AMD IOMMU V2 module is loaded:..............$iommu_exists_result"
echo -e "KFD device exists:..........................$kfd_dev_exists_result"
echo -e "KFD device has correct permissions:.........$kfd_perms_result"
echo -e "Valid GPU ID is detected:...................$gpu_id_result"
echo -e ""
echo -e "Can run HSA.................................$__pass_flag"
