### AMD Heterogenous System Architecture HSA - Linux kfd v0.5 release for Kaveri

### Installation and Configuration guide (v2)

#### What's New in kfd v0.5

* Based on kernel 3.14.0
* Supports running HSA applications in 32bit mode
* Various bug fixes
* Supports both Ubuntu 13.10 and Ubuntu 14.04

#### Package Contents

The kernel archive contains : 

* Ubuntu images:
  * linux-headers-3.14.0-031499_3.14.0-031499.201405041927_all.deb
  * linux-image-3.14.0-031499-generic_3.14.0-031499.201405041927_amd64.deb
  * linux-headers-3.14.0-031499-generic_3.14.0-031499.201405041927_amd64.deb
  * linux-image-extra-3.14.0-031499-generic_3.14.0-031499.201405041927_amd64.deb

* Kaveri firmware image: kaveri-firmware_3.0_all.deb

The kernel image is built from a source tree based on the 3.14 upstream release plus :

* A new HSA kernel driver ("radeon-kfd") which works with the radeon graphics driver.
* Fixes and improvements to the radeon and amd_iommu(v2) drivers, mm and mmu_notifier code.

#### Target Platform

This release is intended for use with a specific hardware configuration :

* APU:            AMD A10-7850K APU
* Motherboard:    ASUS A88X-PRO motherboard (ATX form factor)
* Memory:         G.SKILL Ripjaws X Series 16GB (2 x 8GB) 240-Pin DDR3 SDRAM DDR3 2133
* OS:             Ubuntu 13.10/14.04 64-bit edition
* No discrete GPU present in the system

If the motherboard BIOS version is lower than 0802 it must be updated to 0802 or higher. Nearly all of our testing has been done with the 0802 BIOS although we have lightly tested the 0904 BIOS as well. 

New BIOS versions can be downloaded from the ASUS support pages, starting at the URL below.  After opening the page, click on “Support” in the light gray bar (under the black bar at the top) then click on "Driver & Tools" and choose “Others” from the OS menu.

http://www.asus.com/Motherboards/A88XPRO

You also need to enable IOMMU in the system BIOS. This is done using the “CPU Configuration” screen under “Advanced Mode” 

#### Installing and configuring the kernel

* Downloading the kernel binaries from the repo
  `git clone https://github.com/HSAFoundation/Linux-HSA-Drivers-And-Images-AMD.git`

* Following is the file structure of the repo
  
  * Linux-HSA-Drivers-And-Images-AMD/
      * LICENSE
      * README.md
      * kfd-0.5
        * kaveri-firmware_3.0_all.deb
        * linux-headers-3.14.0-031499_3.14.0-031499.201405041927_all.deb
        * linux-image-3.14.0-031499-generic_3.14.0-031499.201405041927_amd64.deb
        * linux-headers-3.14.0-031499-generic_3.14.0-031499.201405041927_amd64.deb
        * linux-image-extra-3.14.0-031499-generic_3.14.0-031499.201405041927_amd64.deb

* Go to the top of the repo:
  `cd Linux-HSA-Drivers-And-Images-AMD`

* Configure udev to allow any user to access /dev/kfd. As root, use a text editor to create /etc/udev/rules.d/kfd.rules containing one line:  
KERNEL=="kfd", MODE="0666", Or you could use the following command:
  `echo  "KERNEL==\"kfd\", MODE=\"0666\"" | sudo tee /etc/udev/rules.d/kfd.rules`

* Install the linux-image kernel package using:
  `sudo dpkg -i kfd-0.5/*.deb

* Reboot the system to install the new kernel and enable the HSA kernel driver:
  `sudo reboot`
 
#####Obtaining kernel source code 

Source code used to build the kernel can be downloaded with the following command : 
`git clone -b v0.5 git://people.freedesktop.org/~gabbayo/linux.git`

The kernel images were built using Ubuntu mainline kernel PPA patches, which can be downloaded with the following command :
`wget http://people.freedesktop.org/~gabbayo/kfd-v0.5/0001-base-packaging.patch ; wget http://people.freedesktop.org/~gabbayo/kfd-v0.5/0002-debian-changelog.patch ; wget http://people.freedesktop.org/~gabbayo/kfd-v0.5/0003-configs-based-on-Ubuntu-3.14.0-0.1.patch`

Use the instructions in the following wiki page to built the Ubuntu kernel images:
https://help.ubuntu.com/community/Kernel/Compile
