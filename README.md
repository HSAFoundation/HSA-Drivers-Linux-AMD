### AMD Heterogenous System Architecture HSA - Linux Alpha 1 release for Kaveri

### Installation and Configuration guide (v5)

#### Package Contents

The kernel archive contains : 

* Linux kernel image: linux-image-3.13.0-kfd+_3.13.0-kfd+-2_amd64.deb
* Copy of the config file used to build the kernel: 3.13.0-config
* sample xorg.conf file

The kernel image is built from a source tree based on the 3.13 upstream release plus :

* an updated kernel graphics driver ("radeon") including initial dpm support for Kaveri and an interface to support sharing hardware resources with the HSA kernel driver.
* A new HSA kernel driver ("radeon-kfd") which works with the radeon graphics driver.
* Fixes and improvements to the amd_iommu(v2) drivers, mm and mmu_notifier code.

#### Target Platform

This release is intended for use with a specific hardware configuration :

* APU:            AMD A10-7850K APU
* Motherboard:    ASUS A88X-PRO motherboard (ATX form factor)
* Memory:         G.SKILL Ripjaws X Series 16GB (2 x 8GB) 240-Pin DDR3 SDRAM DDR3 2133
* OS:             Ubuntu 13.10 64-bit edition
* No discrete GPU present in the system

If the motherboard BIOS version is lower than 0802 it must be updated to 0802 or higher. Nearly all of our testing has been done with the 0802 BIOS although we have lightly tested the 0904 BIOS as well. 

New BIOS versions can be downloaded from the ASUS support pages, starting at the URL below.  After opening the page, click on “Support” in the light gray bar (under the black bar at the top) then click on "Driver & Tools" and choose “Others” from the OS menu.

http://www.asus.com/Motherboards/A88XPRO

You also need to enable IOMMU in the system BIOS. This is done using the “CPU Configuration” screen under “Advanced Mode” 

#### Installing and configuring the kernel

* Configure udev to allow any user to access /dev/kfd. As root, use a text editor to create /etc/udev/rules.d/kfd.rules containing one line:  
KERNEL=="kfd", MODE="0666"

* Install the linux-image kernel package using:  
  `sudo dpkg -i <package file>`  

* Reboot the system to install the new kernel and enable the HSA kernel driver.
 
##### Enabling SW cursors to fix cursor corruption

The Alpha 1 release has been tested with the standard userspace graphics drivers from Ubuntu 13.10, including the "modesetting" X driver and the "llvmpipe" software-rendering GL driver. The version of modesetting driver in the alpha 1 release does not properly support the 128x128 HW cursor in Kaveri. Fixes have been pushed upstream for the modesetting driver, but the easiest way to eliminate cursor corruption is to enable SW cursors rather than the default HW cursor. 

A sample xorg.conf file is included, which can be copied (as root) into /etc/X11. If you have an existing xorg.conf file you prefer, add the following line to the Device section:  
Option "SWCursor" "yes" 

#####Obtaining kernel source code 

Source code used to build the kernel can be downloaded with the following command : 
`git clone -b kfd-alpha-1 git://people.freedesktop.org/~gabbayo/linux.git`

The kernel config file included with this package should be renamed to .config if you want to duplicate the configuration used to create the kernel packages. 
