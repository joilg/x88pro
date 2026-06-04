# DTS 
add this to your device Tree File

	openvfd {
		compatible = "open,vfd";
		dev_name = "openvfd";
		openvfd_gpio_clk = <&gpio4 RK_PA4 GPIO_ACTIVE_HIGH>;
		openvfd_gpio_stb = <&gpio4 RK_PA5 GPIO_ACTIVE_HIGH>;
		openvfd_gpio_dat = <&gpio4 RK_PA6 GPIO_ACTIVE_HIGH>;
		openvfd_chars = [04 00 01 02 03];
		openvfd_dot_bits = [00 01 02 03 04 05 06];
		openvfd_display_type = <0x03 0x00 0x00 0x00>;
		status = "okay";
	};


# INSTALL Kernel Modul:

sudo dkms add -m openvfd -v 8fadd10
sudo dkms build -m openvfd -v 8fadd10
sudo dkms install -m openvfd -v 8fadd10

# Build Service 
make -C /usr/src/openvfd-8fadd10 OpenVFDService install

# load driver 
modprobe openvfd

# Test Display
Demo Mode:
/usr/sbin/OpenVFDService -dm

Clock:
/usr/sbin/OpenVFDService -24h


Brightness:
echo 6 > /sys/class/leds/openvfd/brightness     0...7

Symbol:
echo wifi > /sys/class/leds/openvfd/led_on     //  ...led_off



#Start Display on Startup
echo openvfd  > /etc/modules-load.d/openvfd.conf
cp /usr/src/openvfd-8fadd10/openvfd.service /etc/systemd/system/
systemctl daemon-reload
systemctl start openvfd.service
systemctl enable openvfd.service

reboot

Start Service:
# Display Time
 OpenVFDService & 
 
 # display Text 
 OpenVFDService -c "Hello" 
 
 