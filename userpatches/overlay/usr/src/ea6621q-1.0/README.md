 # DTS 
add this to your device Tree File

 
 # INSTALL Kernel Modul:

 dkms remove -m ea6621q -v 1.0 && dkms add -m ea6621q -v 1.0 && dkms build -m ea6621q -v 1.0 &&  dkms install -m ea6621q -v 1.0 
 cat /var/lib/dkms/ea6621q/1.0/build/make.log

# load driver 

modprobe skw_sdio
modprobe 
modprobe 
modprobe 


#load module  on Startup
echo skw_sdio  > /etc/modules-load.d/openvfd.conf

reboot



# uninstall:
 dkms remove -m ea6621q -v 1.0
 rmmod
 rmmod
 rmmod
 rmmod skw
 
 
 # test
sudo iw dev wlan0 link

iperf3 -c fritz.box -p 4711

sudo apt install speedtest-cli
speedtest-cli
