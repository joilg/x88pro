# SeekWave EA6521QF / SV6160 Wi-Fi and Bluetooth

This is the existing H96 Max driver port, migrated into Armbian's native
directory-based extension layout.

The driver source archive and firmware checksums are preserved from the
previous mainline project. The extension builds and installs the four
modules through DKMS:

- skw_sdio
- skw_bootcoms
- skw
- skwbt

Firmware files are required locally at build time. They are excluded from
Git because redistribution rights have not been established.

Do not replace this source with an AIC8800 driver. The H96 Max uses the
SeekWave EA6521QF/SV6160 SDIO device.
