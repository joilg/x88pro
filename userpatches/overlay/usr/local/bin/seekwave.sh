#! /bin/sh

sudo dkms add -m ea6x21qx -v 1.0
sudo dkms build -m ea6x21qx -v 1.0
sudo dkms install -m ea6x21qx -v 1.0
