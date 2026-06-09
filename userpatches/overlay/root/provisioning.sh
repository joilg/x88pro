#!/bin/bash

#################################################
openvfd() {

echo "Install openvfd Display driver"
if dkms status openvfd | grep installed ; then
dkms remove -m openvfd -v 8fadd10  >>/var/log/provisioning
systemctl stop openvfd
rmmod openvfd 2>>/var/log/provisioning | true
fi 
dkms add -m openvfd -v 8fadd10 >>/var/log/provisioning
dkms build -m openvfd -v 8fadd10 >>/var/log/provisioning
dkms install -m openvfd -v 8fadd10 >>/var/log/provisioning
echo openvfd  > /etc/modules-load.d/openvfd.conf
modprobe openvfd

make -C /var/lib/dkms/openvfd/8fadd10/source OpenVFDService install >>/var/log/provisioning

systemctl enable openvfd.service
systemctl start openvfd.service
echo 7 >   /sys/class/leds/openvfd/brightness
systemctl enable vfd-network.service
systemctl start vfd-network.service 

}
#####################################################
ea6621q() {
if  [ -d "/sys/kernel/debug/mmc2/mmc2:8800" ]; then  
echo "Seekwave Wifi6 Chip detected"
echo "Install Seekwave EA6521 driver"

#  Chip Name     Driver     DKMS-Version   wifi chip id :
#	LGX8800G		aic8800-sdio     5.0          ac2a    
#	EA6521QF   		ea6621q            1.0          8800   
if  dkms status ea6621q | grep installed; then
dkms remove  -m ea6621q -v 1.0
fi

# dkms remove -m ea6621q -v 1.0  | true
dkms add -m ea6621q -v 1.0 >>/var/log/provisioning
dkms build -m ea6621q -v 1.0 >>/var/log/provisioning
dkms install -m ea6621q -v 1.0 >>/var/log/provisioning

modprobe skw_sdio
modprobe skw_bootcoms
modprobe skw
modprobe skwbt


cat <<EOF > /etc/modules-load.d/skw.conf
hidp
rfcomm
bnep
skw_sdio
skw_bootcoms
skw
skwbt
EOF

echo "Seekwave EA6521 driver installed. Use armbian-config to connect via Wi-Fi" 

fi
}
####################################################
firmware() {
echo "Install Seekwave Firmware"

mkdir /usr/lib/firmware/skw
cp dkms-ea6x21q/ea6x21p/firmware/*  /usr/lib/firmware/skw/
}

###################################################
blacklist() {
if  [ -d "/sys/kernel/debug/mmc2/mmc2:8800" ]; then  
echo
echo "Wifi Chip = ea6521    blacklist AIC8800 Driver "
mv /etc/modules-load.d/aic8800-btlpm.conf /etc/modules-load.d/aic8800-btlpm.bak 2>>/var/log/provisioning 
echo "blacklist aic8800_fdrv" > /etc/modprobe.d/blacklist-aic8800.conf 
echo "blacklist aic8800_btlpm" >> /etc/modprobe.d/blacklist-aic8800.conf 
echo "blacklist aic8800_bsp" >> /etc/modprobe.d/blacklist-aic8800.conf 
echo 
fi
}


###########################################################
echo "Provisioning started"
echo " Provisioning $(date '+%B %V %T.%3N:')   "    >/var/log/provisioning
echo
openvfd
blacklist
# firmware  # already installed
ea6621q

echo "Please reboot to apply the changes."
echo 
echo "Provisioning complete. log info in /var/log/provisioning"



