# SPDX-License-Identifier: GPL-2.0-or-later
# GPIO4_C3 is SDA; GPIO4_C4 is SCL. The FD6551 does not acknowledge.
# Select GPIO function for both pins in the RK3528 IOC.
mw.l 0xff550090 0xf0000000
mw.l 0xff550094 0x000f0000
setenv fd_send 'for fd_bit in ${fd_byte}; do gpio clear E20; if test "${fd_bit}" = "1"; then gpio set E19; else gpio clear E19; fi; gpio set E20; done; gpio clear E20; gpio set E19; gpio set E20; gpio clear E20'
setenv fd_write 'gpio set E19; gpio set E20; gpio clear E19; setenv fd_byte ${fd_addr}; run fd_send; setenv fd_byte ${fd_data}; run fd_send; gpio clear E19; gpio set E20; gpio set E19'

# Enable the display at brightness 3.
setenv fd_addr '0 1 0 0 1 0 0 0'
setenv fd_data '0 0 0 0 1 0 1 1'
run fd_write

# Clear all icons, then write "boot" from the rightmost digit.
setenv fd_addr '0 1 1 0 0 1 1 0'
setenv fd_data '0 0 0 0 0 0 0 0'
run fd_write
setenv fd_addr '0 1 1 0 1 0 0 0'
setenv fd_data '0 1 1 1 1 0 0 0'
run fd_write
setenv fd_addr '0 1 1 0 1 0 1 0'
setenv fd_data '0 1 0 1 1 1 0 0'
run fd_write
setenv fd_addr '0 1 1 0 1 1 0 0'
run fd_write
setenv fd_addr '0 1 1 0 1 1 1 0'
setenv fd_data '0 1 1 1 1 1 0 0'
run fd_write

# Flash USB once, then leave it on for the Linux handover.
setenv fd_addr '0 1 1 0 0 1 1 0'
setenv fd_data '0 0 0 0 0 0 1 0'
run fd_write
sleep 0.25
setenv fd_data '0 0 0 0 0 0 0 0'
run fd_write
sleep 0.25
setenv fd_data '0 0 0 0 0 0 1 0'
run fd_write

setenv fd_addr
setenv fd_bit
setenv fd_byte
setenv fd_data
setenv fd_send
setenv fd_write
