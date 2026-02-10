# sudo dkms remove -m tm1628 -v 1.0 | true
sudo dkms add -m tm1628 -v 1.0
# sudo dkms build -m tm1628 -v 1.0
sudo dkms install -m tm1628 -v 1.0

sudo armbian-add-overlay /usr/src/tm1628-1.0/rk3528_tm1628.dts
echo tm1628  > /etc/modules-load.d/tm1628
reboot


#vfd_gpio_clk='4,4,0' 
#vfd_gpio_dat='4,6,0'
#vfd_gpio_stb='4,5,0'
#vfd_chars='4,3,2,1,0'
#vfd_dot_bits='0,1,3,2,4,5,6'
#vfd_display_type='0x01,0x00,0x00,0x00'
