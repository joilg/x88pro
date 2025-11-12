# Armbian for X88PRO13 TV Box

### WORK IN PROGRESS


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
tested with Armbian GIT revision f2c908119d39e2c385c3ad6aa005cd075fd4eaf7
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

	Write the image with a tool like USBImager or balenaEtcher to your micro-SD card
	Insert mmc into the mmc slot on TV Box

# Usage

	Plugin Power to the TV Box. Follow one of three options to log into Armbian console.

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

	

## Maintainers

[@joilg](https://github.com/joilg).

## Contributing

Feel free contribute.  


## License

[MIT](LICENSE) © joilg
