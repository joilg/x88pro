#!/bin/bash

echo "Provisioning started"

echo "install openvfd Display driver"


dkms add -m openvfd -v 8fadd10
dkms build -m openvfd -v 8fadd10
dkms install -m openvfd -v 8fadd10
echo openvfd  > /etc/modules-load.d/openvfd.conf
modprobe openvfd

make -C /var/lib/dkms/openvfd/8fadd10/source OpenVFDService install


#  wifi chip id :
#				aic8800     ac2a
#				ea6521      8800

if  [ -d "/sys/kernel/debug/mmc2/mmc2:8800" ]; then  
echo "Seekwave Wifi6 detected, Install driver "

systemctl start openvfd.service

systemctl enable openvfd.service


# dkms remove -m ea6x21qx -v 1.0  | true
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

fi

echo "Provisioning complete"
