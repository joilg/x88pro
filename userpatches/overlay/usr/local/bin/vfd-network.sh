#!/bin/bash
 # OpenVFD Automation Script for LAN and Wi-Fi Icons
while true
do
if ip link show end0 2>/dev/null | grep -q "state UP"; then 
	echo eth > /sys/class/leds/openvfd/led_on
else 
	echo eth > /sys/class/leds/openvfd/led_off
fi
# Check Wi-Fi status 
if ip link show wlan0  2>/dev/null | grep -q "state UP"; then
	echo wifi > /sys/class/leds/openvfd/led_on
else 
	echo wifi > /sys/class/leds/openvfd/led_off
fi 
sleep 5 

done 
