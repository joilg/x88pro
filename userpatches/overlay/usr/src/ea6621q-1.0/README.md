# ea6x21-dkms
Seekwave SV6160 / SWT6652 Wi-Fi 6 & Bluetooth Driver (DKMS)

# Seekwave Wifi6  (ea6x21q) Linux Driver 

The driver was tested on a X88PRO13 TV box  under Armbian  with RK3525 CPU and EA6521 Wifi Chip. 
It should be compatible with other WiFi SDIO adapters with the same chip of EA6x21 inside.  
Orginal driver Software from 

A Linux Kernel  driver port for the **Seekwave SV6160 / SWT6x51 ** / combo Wi-Fi 6 and Bluetooth chipsets.

Optimized for Single Board Computers (SBCs) and TV Boxes running on Rockchip SoCs (e.g., **RK3528**, RK3562, RK3566, RK3588) such as the **X88 PRO 13**.

This repository contains critical modernization fixes over vendor SDKs, including Linux 6.1+ DMA-API scatterlist mapping and dual-subsystem (Wi-Fi + BT) GPIO-sharing resource allocation.

## 🛠️ Key Improvements in this Fork

* **Modern Kernel Compatibility:** Fixed silent host lockups and crashes on Kernel 5.15/6.1+ by implementing explicit `dma_map_sg` and `dma_unmap_sg` mapping inside the SDIO data transmission loop.
* **Dual-Subsystem GPIO Sharing:** Patched the DT boot parser logic (`seekwave_boot_parse_dt`) to catch `-EBUSY (-16)` errors. This allows the Wi-Fi and Bluetooth driver threads to safely share hardware initialization pins (`CHIP_EN`, `CHIP_WAKE`, `HOST_WAKE`) rather than conflicting and crashing the Bluetooth stack.
* **Stability Fixes:** Enhanced internal handshaking timers during Wifi-Processor (`CP`) boot loader initialization sequence to prevent common `-62 (-ETIME)` firmware loading timeouts.

---

## 📋 Prerequisites & Requirements


### 2. Linux Kernel  Configuration

Thekernel should has a similar configuration as below.  This is default config in Armbian or Debian
```
[*] Networking support --->
      <*>   Wireless --->
              <*>   cfg80211 - wireless configuration API
              <*>   Generic IEEE 802.11 Networking Stack (mac80211)
      <*>   RF switch subsystem support --->
    Device Drivers --->
      [*] Network device support --->
            [*]   Wireless LAN --->
      [*] Staging drivers --->
            <M>   Support for rtllib wireless devices
            <M>     Support for rtllib CCMP crypto
            <M>     Support for rtllib TKIP crypto
            <M>     Support for rtllib WEP crypto
```


### 2. Device Tree (DTS) Node Configuration
The Seekwave driver relies on explicit NVRAM definitions in your device tree to locate calibration assets. Ensure your wireless MMC/SDIO node looks similar to this:
this example is for the X88PRO13 TV box.  ** modyfy this acording our Board specific Hardware **

```dts
&sdio0 {
	max-frequency = <150000000>;
	no-sd;
	no-mmc;
	supports-sdio;
	bus-width = <4>;
	disable-wp;
	cap-sd-highspeed;
	cap-sdio-irq;
	keep-power-in-suspend;
	non-removable;
	mmc-pwrseq = <&sdio_pwrseq>;
	pinctrl-names = "default";
	pinctrl-0 = <&sdio0_bus4 &sdio0_cmd &sdio0_clk>;
	/delete-property/ rockchip,use-v2-tuning;
	sd-uhs-sdr104;
	status = "okay";

	seekwcn_boot>;
		compatible = "seekwave,sv6160";
		dma_type = <0x01>;
		skw_iram_path = "/lib/firmware/SWT6621_IRAM_SDIO.bin";
		skw_dram_path = "/lib/firmware/SWT6621_DRAM_SDIO.bin";
		bt_antenna = <0>;   /* no BT_antenna setting */
		seekwave_nv_name = "SEEKWAVE_NV_SWT6652.bin";
		gpio_host_wake = <50>;                      // ** Insert here your Board specific GPIO ** 
		gpio_chip_wake = <49>;                       // ** Insert here your Board specific GPIO ** 
		gpio_chip_en =	  <38>;                       // ** Insert here your Board specific GPIO ** 
		pinctrl-names = "default";
		status = "okay";
	};
};
```

2. #### or apply this device tree overlay:   
```
/dts-v1/;
/plugin/;
/ {
	compatible = "rockchip,rk3528";

    fragment@0 {
        target = <&seekwcn_boot>;
        __overlay__ {
			compatible = "seekwave,sv6160";
			dma_type = <0x01>;
			skw_iram_path = "/lib/firmware/SWT6621_IRAM_SDIO.bin";
			skw_dram_path = "/lib/firmware/SWT6621_DRAM_SDIO.bin";
			bt_antenna = <0>;   /* no BT_antenna setting */
			// seekwave_nv_name = "SEEKWAVE_NV_SWT6652.bin";
			gpio_host_wake = <50>;
			gpio_chip_wake = <49>; 
			gpio_chip_en =	  <38>; 
			pinctrl-names = "default";
			status = "okay";
		};
	};
};

```
Apply overlay with 
```
  sudo armbian-add-overlay rk35xx_openvfd.dts 
  sudo reboot  
```

## 🚀 Installation via DKMS

1. Clone this repository directly onto your device:
```
   git clone https://github.com/joilg/dkms-ea6x21.git
   sudo cp -r ea6x21-dkms/ea6x21p-1.0 /usr/src/
```

2. Firmware and Calibration Assets Placement
Pplace your vendor calibration binaries into the core Linux system firmware directories:
```
sudo mkdir /usr/lib/firmware/skw
sudo cp dkms-ea6x21q/ea6x21p/firmware/*  /usr/lib/firmware/skw/

```

3. Register the driver source code directory tree with DKMS:
```
sudo dkms add -m ea6x21q -v 1.0
```

4. Build the modified module binaries against your active system kernel headers:
```
sudo dkms build -m ea6x21q -v 1.0
```

4. Install the module into the active kernel environment:
```
 sudo dkms install -m ea6x21q -v 1.0 
```
5. load modules  
```
modprobe skw_sdio
modprobe skw_bootcoms
modprobe skw
modprobe skwbt
```
6. for automatic load at startup insert modulenames /etc/modules-load.d/skw.conf
```
cat <<EOF > /etc/modules-load.d/skw.conf
hidp
rfcomm
bnep
skw_sdio
skw_bootcoms
skw
skwbt
EOF
```
Reload your system parameters or restart your system:
```
   sudo reboot
```

---

## 📊 Verification & Diagnostics

Once your device boots back up, check your kernel system log to confirm the deployment states.

### Check SDIO 
```
sudodmesg | grep SDIO
	A successful configuration shows
       kernel: mmc2: new ultra high speed SDR104 SDIO card at address 8800
```

### 🌐 Wi-Fi Subsystem Check
Run `dmesg | grep -E "SKW"` to observe the initialization steps. A successful layout shows:
```text
[SKWSDIO INFO] check_chipid: Chip id:SV6160 used SDIO10
[SKWSDIO INFO] skw_sdio_boot_cp: DOWNLOAD BIN TO CP
[SKWSDIO INFO] skw_sdio_handle_packet: LOOPCHECK channel received: WIFIREADY
[SKWBOOT]:skw_start_wifi_service wifi boot sucessfull
[SKWIFI DBG] skw_iface_setup: STA, addr: fe:fd:fc:xx:xx:xx
```

Verify your network interface state:
```
ip link show wlan0
```
It should report `<BROADCAST,MULTICAST,UP,LOWER_UP>` with a dynamic IP assigned by your router.

use your linux Systemcommands   to connect to WIFI accesspoint
Armbian OS use ** armbian-config **


### 🔷 Bluetooth Subsystem Check
Run `hciconfig` to make sure your host controller interface configuration is fully deployed:
```
hciconfig
```
A correct execution output must state:
```text
hci0:   Type: Primary  Bus: SDIO
        BD Address: BC:9A:98:XX:XX:XX  ACL MTU: 1021:9  SCO MTU: 255:4
        UP RUNNING
```

Use `bluetoothctl` to scan for neighboring hardware devices:
```
bluetoothctl
[bluetooth]# power on
[bluetooth]# scan on
```

## uninstall: 
```
dkms remove -m ea6x21q -v 1.0 --all
rm -rf /usr/src/ea6x21q-1.0
```




## 🔍 Troubleshooting

### 1. Error: `skw_check_cp_ready: check CP-ready time out (ret=-62)`
* **Root Cause:** The chip isn't responding fast enough or drawing too much power instantly upon boot.
* **Resolution A:** Many cheap Android TV-Box power supplies (e.g. standard 5V/2A) buckle during active RF transmission bursts. Replace the power brick with a reliable, high-grade **5V/3A** power brick.
* **Resolution B:** Unplug power-demanding USB expansion peripherals (like mechanical hard disks) to optimize voltage thresholds during boot.

### 2. High Ping Jitter or Latenz Spikes
* **Root Cause:** Aggressive hardware power saving state parameters within the vendor driver code.
* **Resolution:** Disable OS-level power saving on the driver instance by executing:
  ```
  sudo iw dev wlan0 set power_save off
  ```

---
## 📝 License
Licensed under the Apache License, Version 2.0 (the "License"). 

---

## Contributing

Feel free to dive in! [Open an issue](https://github.com/joilg/ea6x21-dkms/issues/new) or submit PRs.


