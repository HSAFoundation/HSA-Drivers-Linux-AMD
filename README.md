### AMD Heterogenous System Architecture HSA - Linux kfd v0.8 release for Kaveri

### Installation and Configuration guide

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
  * kaveri-firmware_366_all.deb
  * linux-headers-3.14.11-031450_3.14.11-031450.201408121620_all.deb
  * linux-headers-3.14.11-031450-generic_3.14.11-031450.201408121620_amd64.deb
  * linux-image-3.14.11-031450-generic_3.14.11-031450.201408121620_amd64.deb
  * linux-image-extra-3.14.11-031450-generic_3.14.11-031450.201408121620_amd64.deb

* Fedora images:
  * kernel-3.14.11-1.kfd.f21.x86_64.rpm
  * kernel-headers-3.14.11-1.kfd.f21.x86_64.rpm
  * kernel-modules-extra-3.14.11-1.kfd.f21.x86_64.rpm
  * linux-firmware-20140605_kfd-38.gita4f3bc03.fc21.1.noarch.rpm

* Userspace wrapper library called libhsakmt:
  * lnx/libhsakmt.so.1
  * lnx64a/libhsakmt.so.1

* A bash script which checks if kfd is installed correctly

The kernel image is built from a source tree based on the 3.14.11 upstream
release plus :

* The HSA kernel driver ("radeon-kfd"), which works with the radeon
  graphics driver.
* Fixes and improvements to the radeon and amd_iommu(v2) drivers, mm and
  mmu_notifier code.

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

* Following is the file structure of the repo
  
  * HSA-Drivers-Linux-AMD/
      * LICENSE
      * README.md
      * kfd_check_installation.sh
      * kfd-0.8
        * ubuntu
          * kaveri-firmware_366_all.deb
          * linux-headers-3.14.11-031450_3.14.11-031450.201408121620_all.deb
          * linux-headers-3.14.11-031450-generic_3.14.11-031450.201408121620_amd64.deb
          * linux-image-3.14.11-031450-generic_3.14.11-031450.201408121620_amd64.deb
          * linux-image-extra-3.14.11-031450-generic_3.14.11-031450.201408121620_amd64.deb
        * fedora
          * kernel-3.14.11-1.kfd.f21.x86_64.rpm
          * kernel-headers-3.14.11-1.kfd.f21.x86_64.rpm
          * kernel-modules-extra-3.14.11-1.kfd.f21.x86_64.rpm
          * linux-firmware-20140605_kfd-38.gita4f3bc03.fc21.1.noarch.rpm
        * libhsakmt
          * lnx
            * libhsakmt.so.1
          * lnx64a
            * libhsakmt.so.1

* Go to the top of the repo:
  `cd HSA-Drivers-Linux-AMD`

* Configure udev to allow any user to access /dev/kfd. As root, use a text
editor to create /etc/udev/rules.d/kfd.rules containing one line:
KERNEL=="kfd", MODE="0666", Or you could use the following command:
  `echo  "KERNEL==\"kfd\", MODE=\"0666\"" | sudo tee /etc/udev/rules.d/kfd.rules`

* For Ubuntu, install the linux-image kernel package using:
  `sudo dpkg -i kfd-0.8/ubuntu/*.deb`

* For Fedora, install the kernel package and update initramfs using:
  `sudo yum install kfd-0.8/fedora/*.rpm ; sudo dracut --force --add-drivers "amd_iommu_v2 radeon_kfd" "/boot/initramfs-3.14.11-1.kfd.fc21.x86_64.img" 3.14.11-1.kfd.fc21.x86_64`

* Reboot the system to install the new kernel and enable the HSA kernel driver:
  `sudo reboot`

#####Important note regarding Fedora 21 package
Fedora 21 daily build currently uses kernel 3.16.1
However, kfd v0.8 is based on kernel 3.14.11.
Therefore, our Fedora 21 kernel rpm package is based on 3.14.11 kernel. This
means that there may be some Fedora functionality that would not behave
correctly after our rpm package is installed.

Please also note that a similar approach was used for the previous
Fedora 21 package as well

#####Obtaining kernel source code 

Source code used to build the kernel can be downloaded with the following
command :
`git clone -b v0.8 git://people.freedesktop.org/~gabbayo/linux.git`

For Ubuntu, the kernel images were built using Ubuntu mainline kernel
PPA patches, which can be downloaded with the following command :
`wget http://people.freedesktop.org/~gabbayo/kfd-v0.8/0001-base-packaging.patch ; wget http://people.freedesktop.org/~gabbayo/kfd-v0.8/0002-debian-changelog.patch ; wget http://people.freedesktop.org/~gabbayo/kfd-v0.8/0003-configs-based-on-Ubuntu-3.13.0-8.27.patch ; wget http://people.freedesktop.org/~gabbayo/kfd-v0.8/0004-kfd-changelog.patch`

Use the instructions in the following wiki page to built the Ubuntu kernel images:
https://help.ubuntu.com/community/Kernel/Compile

For Fedora, the kernel images were built using Fedora kernel srpm,
which can be downloaded with the following command :
`wget http://people.freedesktop.org/~gabbayo/kfd-v0.8/kernel-3.14.11-1.kfd.fc21.src.rpm`

Use the instructions in the following wiki page to built the Fedora kernel images:
https://fedoraproject.org/wiki/Building_a_custom_kernel

Alternatively, you can compile the kernel directly by running `make` inside
the kernel directory.
With this method, you will need to use the kernel config file located at:
http://people.freedesktop.org/~gabbayo/kfd-v0.8/config-3.14.11-031450-generic

#####Obtaining firmware binary files

Firmware binary files for kaveri can be downloaded with the following command :
`wget http://people.freedesktop.org/~gabbayo/kfd-v0.8/radeon_ucode.tar.gz`

