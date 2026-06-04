#! /bin/sh

PACKAGE_NAME=TM1628_LED_Driver

#INFO
#https://github.com/venkatesh4009/TM1628_LED_Driver.git
# https://manpages.debian.org/testing/dkms/dkms.8.en.html
#INSTALL
# sudo dkms add -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}
# sudo dkms build -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}


rm -rf TM1628_LED_Driver
#Prepare
git clone https://github.com/venkatesh4009/TM1628_LED_Driver.git
cd TM1628_LED_Driver

PACKAGE_VERSION=$(git log -1 --pretty=format:"%h")
PACKAGE_DIR=$(pwd)

cat > dkms.conf <<EOT
PACKAGE_NAME=${PACKAGE_NAME}
PACKAGE_VERSION=${PACKAGE_VERSION}

BUILT_MODULE_NAME=tm1628
DEST_MODULE_LOCATION="/updates/dkms"
CLEAN="make -C \$kernel_source_dir M=\$dkms_tree/\$PACKAGE_NAME/\$PACKAGE_VERSION/build/Kernel_Driver_tm1628 clean"
MAKE="make -C \$kernel_source_dir M=\$dkms_tree/$PACKAGE_NAME/\$PACKAGE_VERSION/build/Kernel_Driver_tm1628 CONFIG_LEDS_TM1628=m"
AUTOINSTALL=yes
EOT

sudo rm -rf  /usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}
sudo cp -rf  .  /usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}
cd ..

sudo dkms remove -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}
sudo dkms add -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}
sudo dkms build -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}
#  sudo dkms install -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION}

sudo cp /var/lib/dkms/TM1628_LED_Driver/1.0/build/Kernel_Driver_tm1628/tm1628.ko /usr/lib/modules/6.1.115-vendor-rk35xx/kernel/drivers/media/i2c/
sudo bash -c "echo ht1628 > /etc/modules-load.d/ht1628.conf"
 


