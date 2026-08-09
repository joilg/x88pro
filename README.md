# Armbian for X88PRO13 TV Box

### WORK IN PROGRESS


## What’s new: 
09.08.2026
* `x88pro-install-emmc` was successfully tested on an X88 Pro 13 board
  revision 1.7. It transfers a running Armbian system from microSD to the
  internal eMMC while preserving the factory first-stage bootloader.

22.6.2026
* add first support  for current and edge kernel
      no HDMI yet
      LAN, USB ok
      current: only aic8800 wifi 
      edge:     no wifi at all

04.06.2026
*,Wifi also for Seekwave EA6521 Wifi Chip
* Display Driver for HT1628 
* Target OS update to Ubuntu resolute 26.01 


## Background

	X88PRO13 TV box is manufacted by Shenzhen Hugsun Technology Co., Ltd. 
	It is sold under various brand names, eg. LIPA, RUPA X88pro13. Installed operating system is Android 13.
	This repository is used to install and run Armbian operating system on the TV box.



### Specifications:  
    CPU: RK3528 Quad-core 64-bit Cortex-A53
    GPU: Mali 450 MP2
    FLASH: EMMC 16GB
    SDRAM: 2GB
    USB: 1 Type-A USB3.0, 1 Type-A USB2.0
    LAN: Ethernet: 10/100M standard RJ-45
    Bluetooth-compatible: V5.0
    Wireless: 2.4G/5G dual-band WiFi 802.11 a/b/g/n/ac/ax
    Card reader: TF card, maximum support 64GB (not included)
    DisplayPort: HD
    HD: HD 2.0b, for 60Hz 4k
    Power indicator (LED): Standby: red
    Remote control: IR

### Status

HDMI, LAN, WiFi is OK
 
- some Tests required

 

## Building Armbian 

Rockchip rk3528 is now supported by Armbian. Use my patch to build Image for X88Pro13.

### Download Patches and Armbian Build 

```bash
	$ git clone --depth=1 https://github.com/armbian/build build
	$ git clone --depth=1 https://github.com/joilg/x88pro x88pro  
	$ cp -R ./x88pro/userpatches build/
.```

### building server image with console interface

	$ cd build/  
	$ ./compile.sh x88pro

### building desktop image  

	$ cd build  
	$ ./compile.sh x88pro_desktop build BOARD=x88pro BRANCH=vendor BUILD_DESKTOP=yes BUILD_MINIMAL=no DESKTOP_APPGROUPS_SELECTED='browsers desktop_tools editors internet multimedia programming remote_desktop' DESKTOP_ENVIRONMENT=gnome DESKTOP_ENVIRONMENT_CONFIG_NAME=config_base KERNEL_CONFIGURE=no RELEASE=noble

## Install

#### Install Armbian on MicroSD Card

after building, find the compressed image in build/output/images

	$ ls ./output/images/*.img  
	./output/images/Armbian-unofficial_25.11.0-trunk_X88pro_noble_vendor_6.1.115.img  
	./output/images/Armbian-unofficial_25.11.0-trunk_X88pro_noble_vendor_6.1.115_gnome_desktop.img  

	Write the image with Armbian Imager to-SD card
	Insert mmc into the mmc slot on TV Box

# Usage

	Plugin the 5V Powersupply  to the TV Box. Follow one of three options to log into Armbian console.

## 1.) using HDMI monitor and USB Keyboard 

	Connect HDMI to Monitor
	connect USB Keyboard to the USB2 slot.
	Follow instruction from Monitor.  You  have to insert a new root Password.

## 2.) Log into console with serial interface 

	Open Case of the TV box. There are three solder pads on front side of the cuircuit board.
	from corner pin:  TX, RX, GND   3.3V
	Connect 3.3V USB Serial Interface to these Pins
	Use Putty to log in. Baudrate is 1500000 bps

## 3.) connect via ssh

	ssh is enabled by default. Connect network cable to the Box
	$ ssh ssh root@x88pro

#### Update Armbian

	$ arbian-Update


### Enable 2nd USB port as host

### Install Armbian to eMMC

> [!CAUTION]
> Installing to eMMC destroys the existing Android partitions and data. Make a
> complete backup of the factory eMMC/Android installation before proceeding.
> Keep a Maskrom recovery method and `rkdeveloptool` available. The automatic
> backup made by the script covers the boot area and partition table; it is not
> a complete backup of Android.

Do **not** use the standard `armbian-install` or `nand-sata-install` tools on
this board. They overwrite the first-stage bootloader (`idbloader`) with a
generic RK3528 build whose DDR initialization was unstable on the tested box.

The supplied `x88pro-install-emmc` script preserves the factory `idbloader`,
writes only Armbian's second-stage `u-boot.itb`, creates new boot and root
filesystems, and copies the running microSD installation to them. It has been
successfully tested on PCB revision 1.7, but other revisions remain untested.
The factory boot area on the eMMC must still be intact.

Boot Armbian from microSD and review the detected devices and proposed changes:

```bash
sudo x88pro-install-emmc --dry-run
```

If everything is correct, run the interactive installation:

```bash
sudo x88pro-install-emmc
```

The script saves its boot-area backup and partition-table dump under
`/root/x88pro-emmc-backup/` on the microSD system before making destructive
changes. Do not remove that backup until eMMC boot has been tested. When the
copy completes, shut the box down, remove the microSD card, and verify that it
boots from eMMC.

## Maintainers

[@joilg](https://github.com/joilg).

## Contributing

Feel free contribute.  Tests are desired


## License

[MIT](LICENSE) © joilg
