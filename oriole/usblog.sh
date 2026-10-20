#!/bin/bash
# macOS host side of the phone's USB Ethernet (kernel 0129, boot-arg
# sweetusb=2): the macOS counterpart of tools/sweet-usbnet.sh.
#   oriole/usblog.sh [LOGFILE]
# Waits for the phone's CDC-ECM interface (host MAC 02:00:5e:77:ee:01),
# checks that it has 192.168.7.1 (the phone is 192.168.7.2), then prints the
# live kernel console the phone streams to UDP port 5140 (and appends it to
# LOGFILE if given). The address needs root once per attach:
#   sudo ifconfig IF inet 192.168.7.1 netmask 255.255.255.0
# or, persistently, System Settings > Network > (the phone's USB Ethernet) >
# Details > TCP/IP > Manually, 192.168.7.1 / 255.255.255.0.
set -eu
mac=02:00:5e:77:ee:01
ifname() { ifconfig | awk -v mac="$mac" '/^[a-z]/ {i=$1} $1=="ether" && tolower($2)==mac {sub(":","",i); print i}'; }
echo "waiting for the phone's USB Ethernet ($mac)..." >&2
until [ -n "$(ifname)" ]; do sleep 1; done
ifn=$(ifname)
until ifconfig "$ifn" | grep -q "inet 192.168.7.1 "; do
	echo "$ifn has no 192.168.7.1: run  sudo ifconfig $ifn inet 192.168.7.1 netmask 255.255.255.0" >&2
	sleep 5
done
echo "$ifn: 192.168.7.1 <-> phone 192.168.7.2; live log on udp/5140" >&2
if [ $# -ge 1 ]; then
	nc -u -k -l 5140 | tee -a "$1"
else
	nc -u -k -l 5140
fi
