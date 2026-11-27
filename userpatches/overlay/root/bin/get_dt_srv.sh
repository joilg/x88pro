#! /bin/sh

dtc -I fs  -O dts   -@ -s -o /tmp/devicetree.dts /sys/firmware/devicetree/base 
