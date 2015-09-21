### AMD Heterogenous System Architecture (HSA) - Linux amdkfd v1.0 release for Kaveri

### Installation and Configuration guide

#### What's New in kfd v1.0

* Based on newer kernel 3.17.2
* Based on amdkfd code, which is expected to be part of Linux kernel 3.19
* Added two new kernel module parameters:
  * Number of HSA processes - default is 32 and maximum is 512
  * Number of queues per HSA process - default is 128 and maximum is 1024
* Improved Kaveri microcode
* Various bug fixes
* Supports Ubuntu 14.04 and Fedora 21

#### What's New in kfd v0.9

* Driver now supports over-subscription of queues and processes. This means
  that each HSA application can open up to 1024 queues, and there can be up to
  512 HSA applications simultaneously, compared to a total of 23 queues and 8
  HSA processes in previous versions.
* Better support for H/W debug
* New Kaveri microcode which supports AQL queues
* Various bug fixes
* Supports Ubuntu 14.04 and Fedora 21 (with older kernel)

#### What's New in kfd v0.8

* Based on kernel 3.14.11
* Supports HSA signals (kfd events)
* Enable allocating memory from GPU local memory for HSA apps
* Supports Graphics interop (sharing objects between graphic apps and HSA apps)
* Supports SDMA queues
* Supports AQL queue creation
* Add H/W debug support
* Various bug fixes
* Supports Ubuntu 14.04 and Fedora 21 (with older kernel)

#### What's New in kfd v0.6.1

* Based on kernel 3.14.4
* Various bug fixes
* Supports Ubuntu 14.04 and Fedora 21

#### What's New in kfd v0.5.1

* Fix bug in topology code that prevented kfd to load on some Motherboards

#### What's New in kfd v0.5

* Based on kernel 3.14.0
* Supports running HSA applications in 32bit mode
* Various bug fixes
* Supports both Ubuntu 13.10 and Ubuntu 14.04

#### What's New in Alpha 2

* Supports wider range of Kaveri APU types
* Kernel image built with Ubuntu 13.10 stock configuration file
* Improved stability

#### Package Contents

The Linux drivers archive contains :

* Ubuntu images:
  * linux-headers-3.17.2-031450_3.17.2-031450.201412060000_all.deb
  * linux-headers-3.17.2-031450-generic_3.17.2-031450.201412060000_amd64.deb
  * linux-image-3.17.2-031450-generic_3.17.2-031450.201412060000_amd64.deb
  * linux-image-extra-3.17.2-031450-generic_3.17.2-031450.201412060000_amd64.deb
  * radeon-firmware_391_all.deb

* Fedora images:
  * kernel-3.17.2-300.amdkfd.fc21.x86_64.rpm
  * kernel-core-3.17.2-300.amdkfd.fc21.x86_64.rpm
  * kernel-headers-3.17.2-300.amdkfd.fc21.x86_64.rpm
  * kernel-modules-3.17.2-300.amdkfd.fc21.x86_64.rpm
  * kernel-modules-extra-3.17.2-300.amdkfd.fc21.x86_64.rpm
  * linux-firmware-20141013-41.git0e5f6377.fc21.amdkfd.391.noarch.rpm

* Userspace wrapper library called libhsakmt:
  * lnx/libhsakmt.so.1
  * lnx64a/libhsakmt.so.1

* A bash script which checks if kfd is installed correctly

The kernel image is built from a source tree based on the 3.17.2 upstream
release plus :

* The HSA kernel driver ("amdkfd"), which works with the radeon
  graphics driver.
* Fixes and improvements to the radeon and amd_iommu(v2) drivers, mm and
  mmu_notifier code.

##### Note regarding libhsakmt compatibility
Please note that the libhsakmt library in this repository is NOT compatible 
with amdkfd that is distributed as part of the mainline Linux kernel 
from 3.19 and onward.

#### Target Platform

This release is intended for use with any hardware configuration that
contains a Kaveri APU.

The motherboards must support the FM2+ socket, run latest BIOS version
and have the IOMMU enabled in the BIOS.

The following is a reference hardware configuration that was used for
testing purposes:

* APU:            AMD A10-7850K APU
* Motherboard:    ASUS A88X-PRO motherboard (ATX form factor)
* Memory:         G.SKILL Ripjaws X Series 16GB (2 x 8GB) 240-Pin DDR3 SDRAM DDR3 2133
* OS:             Ubuntu 14.04 64-bit edition
* No discrete GPU present in the system

#### Installing and configuring the kernel

* Downloading the kernel binaries from the repo  
`git clone https://github.com/HSAFoundation/HSA-Drivers-Linux-AMD.git`

* Go to the top of the repo:  
`cd HSA-Drivers-Linux-AMD`

* Configure udev to allow any user to access /dev/kfd. As root, use a text
editor to create /etc/udev/rules.d/kfd.rules containing one line:
KERNEL=="kfd", MODE="0666", Or you could use the following command:  
`echo  "KERNEL==\"kfd\", MODE=\"0666\"" | sudo tee /etc/udev/rules.d/kfd.rules`

* For Ubuntu, install the linux-image kernel package using:  
`sudo dpkg -i kfd-1.0/ubuntu/*.deb`

* For Fedora, install the kernel package and update initramfs using:  
`sudo yum install kfd-1.0/fedora/*.rpm ; sudo dracut --force --add-drivers "amd_iommu_v2 amdkfd" "/boot/initramfs-3.17.2-300.amdkfd.fc21.x86_64.img" 3.17.2-300.amdkfd.fc21.x86_64`

* Reboot the system to install the new kernel and enable the HSA kernel driver:  
`sudo reboot`

* Check the HSA driver installation:
`cd HSA-Drivers-Linux-AMD; ./kfd_check_installation.sh`


#####Obtaining kernel and libhsakmt source code

* Source code used to build the kernel can be downloaded with the following
command :  
`git clone -b v1.0 git://people.freedesktop.org/~gabbayo/linux.git`

* The kernel config file used to create the debian packages is located at:  
http://people.freedesktop.org/~gabbayo/amdkfd/kfd-v1.0/config-3.17.2-031450-generic

* Source code used to build the libhsakmt library can be downloaded with the
following command :  
`git clone -b v1.0 git://people.freedesktop.org/~gabbayo/libhsakmt`

* For Ubuntu, the kernel images were built using Ubuntu mainline kernel
PPA patches, which can be downloaded with the following command :  
`wget http://people.freedesktop.org/~gabbayo/amdkfd/kfd-v1.0/0001-base-packaging.patch ; wget http://people.freedesktop.org/~gabbayo/amdkfd/kfd-v1.0/0002-debian-changelog.patch ; wget http://people.freedesktop.org/~gabbayo/amdkfd/kfd-v1.0/0003-configs-based-on-Ubuntu-3.17.0-0.0.patch ; wget http://people.freedesktop.org/~gabbayo/amdkfd/kfd-v1.0/0004-kfd-changelog.patch`

* Use the instructions in the following wiki page to built the Ubuntu kernel images:
https://help.ubuntu.com/community/Kernel/Compile

* For Fedora, the kernel images were built using Fedora kernel srpm,
which can be downloaded with the following command :  
`wget http://people.freedesktop.org/~gabbayo/amdkfd/kfd-v1.0/kernel-3.17.2-300.amdkfd.fc21.src.rpm`

* Use the instructions in the following wiki page to built the Fedora kernel images:  
https://fedoraproject.org/wiki/Building_a_custom_kernel

#####Obtaining firmware binary files

* Firmware binary files for kaveri can be downloaded with the following command :  
`wget http://people.freedesktop.org/~gabbayo/amdkfd/kfd-v1.0/radeon_ucode.tar.gz`

###LICENSE

The following lists the different licenses that apply to the different
components in this repository:

* the Linux kernel images are covered by the modified GPL license in COPYING
* libhsakmt is covered by the X11 license in LICENSE.libhsakmt
* the firmware image is covered by the license in LICENSE.ucode
