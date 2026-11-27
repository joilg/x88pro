#! /bin/sh

sudo apt install iperf -y
iperf -c fritz.box -p 4711
