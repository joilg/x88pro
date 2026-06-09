#! /bin/sh

sudo dkms add -m ea6x21qx -v 1.0
sudo dkms build -m ea6x21qx -v 1.0
sudo dkms install -m ea6x21qx -v 1.0

cp /usr/src/ea6x21qx-1.0/modules/skw*.ko /usr/lib/modules/6.1.115-vendor-rk35xx/updates/dkms/ 
depmod -a


