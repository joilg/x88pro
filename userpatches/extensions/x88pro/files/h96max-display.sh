#!/bin/sh
# FD6551 front-panel display: "boot" with blinking USB icon while Linux boots,
# then HH:MM and link icons.
LED=/sys/class/leds/display
modprobe -q i2c_gpio 2> /dev/null
modprobe -q tm16xx_i2c 2> /dev/null
i=0
while [ ! -d "$LED" ] && [ "$i" -lt 30 ]; do
	sleep 1
	i=$((i + 1))
done
[ -d "$LED" ] || exit 1

echo 3 > "$LED/brightness"
icon() { echo "$2" > "/sys/class/leds/display::$1/brightness"; }
state() { [ "$(cat "$1" 2> /dev/null)" = "up" ] && echo 1 || echo 0; }

for n in alarm play pause colon lan wlan usb; do icon "$n" 0; done
echo boot > "$LED/message"

tick=0
while :; do
	case "$(systemctl is-system-running 2> /dev/null)" in
	running | degraded) break ;;
	esac
	# systemd-backlight restores saved LED states during boot; clear them again
	echo boot > "$LED/message"
	for n in alarm play pause colon lan wlan; do icon "$n" 0; done
	icon usb $((tick % 2))
	tick=$((tick + 1))
	sleep 0.5
done

tick=0
while :; do
	date +%H%M > "$LED/message"
	icon colon $((tick % 2))
	if [ $((tick % 5)) -eq 0 ]; then
		icon wlan "$(state /sys/class/net/wlan0/operstate)"
		icon lan "$(state /sys/class/net/end0/operstate)"
		if ls /sys/bus/usb/devices/*/product > /dev/null 2>&1; then icon usb 1; else icon usb 0; fi
	fi
	tick=$((tick + 1))
	sleep 1
done
