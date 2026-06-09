#!/bin/bash

#################################################
openvfd() {

echo "install openvfd Display driver"
if dkms status openvfd | grep installed ; then
echo "remove old driver"
dkms remove -m openvfd -v 8fadd10
systemctl stop openvfd
rmmod openvfd 2>/dev/null | true
fi 
dkms add -m openvfd -v 8fadd10
dkms build -m openvfd -v 8fadd10
dkms install -m openvfd -v 8fadd10
echo openvfd  > /etc/modules-load.d/openvfd.conf
modprobe openvfd

make -C /var/lib/dkms/openvfd/8fadd10/source OpenVFDService install

systemctl enable openvfd.service
systemctl start openvfd.service
echo 7 >   /sys/class/leds/openvfd/brightness
}
#####################################################
ea6621q() {
#  Chip Name     Driver     DKMS-Version   wifi chip id :
#	LGX8800G		aic8800-sdio     5.0          ac2a    
#	EA6521QF   		ea6621q            1.0          8800   

if  dkms status ea6621q | grep installed; then
echo "uninstall current driver"
dkms remove  -m ea6621q -v 1.0
fi

# dkms remove -m ea6621q -v 1.0  | true
dkms add -m ea6621q -v 1.0
dkms build -m ea6621q -v 1.0
dkms install -m ea6621q -v 1.0

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
}

firmware() {
mkdir /usr/lib/firmware/skw
cp dkms-ea6x21q/ea6x21p/firmware/*  /usr/lib/firmware/skw/
}

blacklist() {
if  [ -d "/sys/kernel/debug/mmc2/mmc2:8800" ]; then  
echo
echo "Wifi Chip = ea6521    blacklist AIC8800 Driver "
mv /etc/modules-load.d/aic8800-btlpm.conf /etc/modules-load.d/aic8800-btlpm.bak
echo "aic8800_fdrv" > /etc/modprobe.d/blacklist-aic8800.conf
echo "aic8800_btlpm" >> /etc/modprobe.d/blacklist-aic8800.conf
echo "aic8800_bsp" >> /etc/modprobe.d/blacklist-aic8800.conf
echo 
fi



###########################################################
echo "Provisioning started"
echo

# WIFI
echo "install openvfd Display driver"
openvfd

if  [ -d "/sys/kernel/debug/mmc2/mmc2:8800" ]; then  
echo "Seekwave Wifi6 detected"
echo "echo "install ea6621q WIFI driver" driver"
ea6621q
echo "Seekwave EA6521 driver installed; please reboot for using it"

echo "Provisioning complete"



