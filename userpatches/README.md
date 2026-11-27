X88pro13 TV testet with  box  and Armbian build  branch v26.8.0-trunk.121

git clone --branch v26.11.0-trunk.140 https://github.com/armbian/build build
git clone  https://github.com/joilg/x88pro x88pro  
cp -R ./x88pro/userpatches build/
cd build/  

./compile.sh BOARD=x88pro    # select kernel branch, OS release, desktop image  
./compile.sh x88pro     # edge 
./compile.sh x88prov	# vendor 
Version 0.8

+++vendor+++										
BOOT				yes
VERSION				v26.11 rolling
KERNEL				Linux x88pro 6.1.172-vendor-rk35xx
NETWORK				100Mbs
WIRELESS PCBv1.1	yes
WIRELESS PCBv1.5	yes
WIRELESS PCBv1.7	?       *)
USB					500M 						
HDMI				yes
IR-RemoteC			yes
armbian-debug:		https://paste.armbian.com/cufemawafesabatu		


+++edge+++
BOOT				yes
VERSION				v26.11 rolling
KERNEL				Linux x88pro 7.3.6-edge-rockchip64
NETWORK				100Mbs
WIRELESS PCBv1.1	yes
WIRELESS PCBv1.5	yes
WIRELESS PCBv1.7	?
USB					500M 						
HDMI				yes
IR-RemoteC			yes

armbian-debug:		https://paste.armbian.com/difohavajuhefoja

