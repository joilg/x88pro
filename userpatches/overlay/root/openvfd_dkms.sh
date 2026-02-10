#! /bin/sh
set -x
# set -e
PACKAGE_NAME=openvfd

#TODO:
# https://github.com/jefflessard/tm16xx-display/tree/main

#INFO
# https://forum.armbian.com/topic/55312-install-openvfd-for-lcd-display-on-recent-612-kernels-tutorial/

#INSTALL
# sudo dkms add -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}
# sudo dkms build -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}

#Prepare
mkdir -p ~/src
cd ~/src
rm -rf $PACKAGE_NAME
git clone https://github.com/arthur-liberman/linux_openvfd.git  $PACKAGE_NAME
cd $PACKAGE_NAME/driver
PACKAGE_VERSION=$(git log -1 --pretty=format:"%h")
PACKAGE_DIR=$(pwd)

cat << 'EOF' >Makefile
obj-m := openvfd.o
openvfd-objs += protocols/i2c_sw.o
openvfd-objs += protocols/i2c_hw.o
openvfd-objs += protocols/spi_sw.o
openvfd-objs += controllers/dummy.o
openvfd-objs += controllers/seg7_ctrl.o
openvfd-objs += controllers/fd628.o
openvfd-objs += controllers/fd650.o
openvfd-objs += controllers/hd44780.o
openvfd-objs += controllers/gfx_mono_ctrl.o
openvfd-objs += controllers/ssd1306.o
openvfd-objs += controllers/pcd8544.o
openvfd-objs += controllers/il3829.o
openvfd-objs += openvfd_drv.o
EOF

cat > dkms.conf << EOT
PACKAGE_NAME=${PACKAGE_NAME}
PACKAGE_VERSION=${PACKAGE_VERSION}
AUTOINSTALL=yes
CLEAN="make -C \$kernel_source_dir M=\$dkms_tree/\$PACKAGE_NAME/\$PACKAGE_VERSION/build clean"
MAKE="make -C \$kernel_source_dir M=\$dkms_tree/$PACKAGE_NAME/\$PACKAGE_VERSION/build"

BUILT_MODULE_NAME="/openvfd"
DEST_MODULE_LOCATION="/updates/dkms"
EOT

sudo rm -rf  /usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}
sudo mkdir -p  /usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}
sudo cp -rfv  ./  /usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}/

# find  /usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}

sudo dkms remove -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION} | true
sudo dkms add -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}
# sudo dkms build -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}
sudo dkms install -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}

#/var/lib/dkms/linux_openvfd/dd45249/build
sudo cp /var/lib/dkms/$PACKAGE_NAME/$PACKAGE_VERSION/build/*.ko  /usr/lib/modules/6.1.115-vendor-rk35xx/updates/dkms/

sudo mkdir /usr/src/overlay
sudo bash -c "cat << 'EOF' >rk3528_openvfd.dts
/dts-v1/;
/plugin/;

/ {
	compatible = "rockchip,rk3528";

	fragment@0 {
		target = <&openvfd>;
		__overlay__ {
			compatible = "open,vfd";
			dev_name = "openvfd";
			status = "okay";
			vfd_gpio_clk='4,4,0' 
			vfd_gpio_dat='4,6,0'
			vfd_gpio_stb='4,5,0'
			vfd_chars='4,3,2,1,0'
			vfd_dot_bits='0,1,3,2,4,5,6'
			vfd_display_type='0x03,0x00,0x00,0x00'
		};
	};
};
EOF"


sudo armbian-add-overlay rk3528_openvfd.dts

#modinfo:
#parm:           vfd_gpio_chip_name:charp
#parm:           vfd_gpio_clk:array of uint
#parm:           vfd_gpio_dat:array of uint
#parm:           vfd_gpio_stb:array of uint
#parm:           vfd_gpio0:array of uint
#parm:           vfd_gpio1:array of uint
#parm:           vfd_gpio2:array of uint
#parm:           vfd_gpio3:array of uint
#parm:           vfd_gpio_protocol:array of uint
#parm:           vfd_chars:array of uint
#parm:           vfd_dot_bits:array of uint
#parm:           vfd_display_type:array of uint
#parm:           vfd_display_auto_power:byte
#parm:           vfd_brightness:uint

sudo bash -c "cat << 'EOF' > /etc/openvfd.conf
vfd_gpio_clk='4,4,0' 
vfd_gpio_dat='4,6,0'
vfd_gpio_stb='4,5,0'
vfd_chars='4,3,2,1,0'
vfd_dot_bits='0,1,3,2,4,5,6'
vfd_display_type='0x03,0x00,0x00,0x00'
EOF"


sudo bash -c "cat << 'EOF' > /etc/modprobe.d/openvfd.conf
vfd_gpio_clk='4,4,0' 
vfd_gpio_dat='4,6,0'
vfd_gpio_stb='4,5,0'
vfd_chars='4,3,2,1,0'
vfd_dot_bits='0,1,3,2,4,5,6'
vfd_display_type='0x03,0x00,0x00,0x00'
EOF"




sudo bash -c "echo openvfd > /etc/modules-load.d/openvfd.conf"
sudo depmod -a

exit 
#sudo cp /var/lib/dkms/TM1628_LED_Driver/1.0/build/Kernel_Driver_tm1628/tm1628.ko /usr/lib/modules/6.1.115-vendor-rk35xx/kernel/drivers/media/i2c/
#sudo bash -c "echo ht1628 > /etc/modules-load.d/ht1628.conf"
 dmesg:
 [146456.513595] OpenVFD: Version: V1.4.4
[146456.513624] OpenVFD: vfd_gpio_clk:          Empty.
[146456.513636] OpenVFD: vfd_gpio_dat:          Empty.
[146456.513646] OpenVFD: vfd_gpio_stb:          Empty.
[146456.513662] OpenVFD: vfd_gpio0:             #0 = 0x00; #1 = 0x00; #2 = 0xFF;
[146456.513677] OpenVFD: vfd_gpio1:             #0 = 0x00; #1 = 0x00; #2 = 0xFF;
[146456.513692] OpenVFD: vfd_gpio2:             #0 = 0x00; #1 = 0x00; #2 = 0xFF;
[146456.513707] OpenVFD: vfd_gpio3:             #0 = 0x00; #1 = 0x00; #2 = 0xFF;
[146456.513720] OpenVFD: vfd_gpio_protocol:     #0 = 0x00; #1 = 0x00;
[146456.513731] OpenVFD: vfd_chars:             Empty.
[146456.513741] OpenVFD: vfd_dot_bits:          Empty.
[146456.513751] OpenVFD: vfd_display_type:      Empty.
[146456.513770] OpenVFD: Detected gpio chips:   gpio0, gpio1, gpio2, gpio3, gpio4.
[146456.513779] OpenVFD: Failed to verify VFD configuration file, attempt using device tree as fallback.
[146456.520211] OpenVFD: openvfd_gpio_clk pin entry not found
[146456.520229] OpenVFD: openvfd_gpio_dat pin entry not found
[146456.520240] OpenVFD: openvfd_gpio_stb pin entry not found
[146456.520249] OpenVFD: openvfd_gpio0 pin entry not found
[146456.520259] OpenVFD: openvfd_gpio1 pin entry not found
[146456.520268] OpenVFD: openvfd_gpio2 pin entry not found
[146456.520278] OpenVFD: openvfd_gpio3 pin entry not found
[146456.520287] OpenVFD: can't find openvfd_chars list, falling back to defaults.
[146456.520295] OpenVFD: chars_prop = 0000000000000000
[146456.526554] OpenVFD: can't find openvfd_dot_bits list, falling back to defaults.
[146456.526570] OpenVFD: dot_bits_prop = 0000000000000000
[146456.532804] OpenVFD: display.type = 0, display.controller = 0, pdata->dev->dtb_active.display.flags = 0x00
[146456.532824] OpenVFD: can't request gpio of gpio_clk
[146456.532840] openvfd: probe of openvfd failed with error -22



