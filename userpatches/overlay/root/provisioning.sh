#!/bin/bash

echo "Provisioning started V1.2  $(date '+%B %V %T.%3N:')"

install -d -m 0755 "/usr/bin"
install -d -m 0755 "/usr/lib/systemd/system"

if [ "$(uname -r | cut -c1-4)" = "6.1." ]; then
	echo "Install openvfd Display driver"
	if dkms status openvfd | grep installed ; then
		dkms remove -m openvfd -v 8fadd10  
		systemctl stop openvfd
		rmmod openvfd 2>> /dev/null | true
	fi
	dkms add -m openvfd -v 8fadd10
	dkms build -m openvfd -v 8fadd10  
	dkms install -m openvfd -v 8fadd10

	build_dkms openvfd 8fadd10
	echo openvfd  > /etc/modules-load.d/openvfd.conf
	make -C /var/lib/dkms/openvfd/8fadd10/source OpenVFDService install 
	modprobe openvfd
	install -m 0644 "/usr/src/bsp/openvfd/openvfd.service" "/usr/lib/systemd/system/openvfd.service"
	install -m 0644 "/usr/src/bsp/openvfd/vfd-network.service" "/usr/lib/systemd/system/vfd-network.service"
	systemctl enable openvfd.service
	systemctl start openvfd.service
	systemctl enable vfd-network.service
	systemctl start vfd-network.service 
	echo 7 >   /sys/class/leds/openvfd/brightness
else
	install -m 0644 "/usr/src/bsp/openvfd/tm16xx.conf" "/etc/modules-load.d/tm16xx.conf"
	install -m 0755 "/usr/src/bsp/openvfd/display-service" "/usr/sbin/display-service"
	install -m 0644 "/usr/src/bsp/openvfd/display.service" "/usr/lib/systemd/system/display.service"
	modprobe tm16xx_spi
	systemctl daemon-reload  >>/var/log/provisioning
	systemctl enable display    
	systemctl restart display
fi


DEVICE_ID=$(cat /sys/bus/sdio/devices/*/device 2>/dev/null | head -n 1)
#  Chip Name	Driver     DKMS-Version   address	chip id 
#	LGX8800G	aic8800-sdio    5.0		ac2a    0x0145
#	EA6521QF	ea6x21-dkms		6.1    	8800    0x0000  vendor kvers 6.1
#	EA6521QF	ea6621q         1.0		8800    0x0000  current, edge > 6.18
#   EA6521S		ea6621s			1.01	0001    0x6621

case "$DEVICE_ID" in
    "0x0145"|"0x0146")
        #AIC8800 WIFI
		echo "AIC8800 Wifi6 - InstallChip AIC8800 wifi driver"
		rm -f /etc/modules-load.d/ea6621q.conf 
		rm -f /etc/modules-load.d/ea6621s.conf 
		install -m 0644 "/usr/src/bsp/aic8800/aic8800.conf" "/etc/modules-load.d/aic8800-btlpm.conf"
		install -m 0644 "/usr/src/bsp/aic8800/options-aic8800-btlpm.conf" "/etc/modprobe.d/aic8800-btlpm.conf"
		install -m 0755 "/usr/src/bsp/aic8800/aic-bluetooth" "/usr/bin/aic-bluetooth"
		install -m 0644 "/usr/src/bsp/aic8800/aic-bluetooth.service" "/usr/lib/systemd/system/aic-bluetooth.service"
		;;
    "0x0000")
	   # Seekwave EA6521q
	    echo "Wifi Chip = ea6521q Install Seekwave EA6521q driver"
		modprobe -r -a swt6621s_wifi skw_sdio_lite  2>> /dev/null | true
		dkms status swt6621s/1.1.0 | grep installed && dkms remove swt6621s/1.1.0 2>> /dev/null | true
		dkms status ea6621q/1.0 | grep installed || dkms install ea6621q/1.0 2>> /dev/null | true
		rm -f /etc/modules-load.d/ea6621s.conf 
		rm -f /etc/modules-load.d/aic8800.conf 
		rm -f /etc/modprobe.d/ea6621s.conf 
		rm -f /etc/modprobe.d/aic8800-btlpm.conf 
		install -m 0644 "/usr/src/bsp/ea6621/ea6621q.conf" "/etc/modules-load.d/ea6621q.conf"
		install -m 0644 "/usr/src/bsp/ea6621/options-ea6621q.conf" "/etc/modprobe.d/ea6621q.conf"
		modprobe -a skw_sdio skw_bootcoms skw skwbt
 		echo "Seekwave EA6521q driver installed"
        ;;
    "0x6621")
	   # Seekwave EA6521s
	    echo "Seekwave Wifi6 Chip detected"
		echo "Wifi Chip = ea6521    blacklist AIC8800 Driver "
		modprobe -r -a skw skwbt skw_bootcoms skw_sdio 2>> /dev/null | true
		dkms status ea6621q/1.0 | grep installed && dkms remove ea6621q/1.0 2>> /dev/null | true
		dkms status swt6621s/1.1.0 | grep installed || dkms install swt6621s/1.1.0 2>> /dev/null | true
		rm -f /etc/modules-load.d/ea6621q.conf 
		rm -f /etc/modules-load.d/aic8800.conf 
		rm -f /etc/modprobe.d/ea6621q.conf 
		rm -f /etc/modprobe.d/aic8800-btlpm.conf 
		install -m 0644 "/usr/src/bsp/ea6621/ea6621s.conf" "/etc/modules-load.d/ea6621s.conf"
		install -m 0644 "/usr/src/bsp/ea6621/options-ea6621s.conf" "/etc/modprobe.d/ea6621s.conf"
		modprobe -a skw_sdio_lite swt6621s_wifi
       ;;
    *)
        echo "Unbekannter SDIO Chip oder kein Gerät gefunden: $DEVICE_ID" 
        ;;
esac

#hold linux-dtb packages on update until x88pro will be supported on armbian build
apt-mark hold linux-dtb-vendor-rk35xx 
apt-mark hold linux-dtb-edge-rockchip64
apt-mark hold linux-dtb-current-rockchip64

#prevent user from install to eMMC via Armbian config/install
#sudo mv /usr/sbin/nand-sata-install /usr/sbin/nand-sata-install.orig
#sudo nano /usr/sbin/nand-sata-install
#!/bin/bash
#whiptail --title "Funktion deaktiviert" --msgbox "Bitte nutzen Sie das vorinstallierte Spezial-Tool für die eMMC-Installation." 10 60
#exit 1
#sudo chmod +x /usr/sbin/nand-sata-install
#sudo chattr +i /usr/sbin/nand-sata-install

echo
echo "Provisioning complete."
echo 



