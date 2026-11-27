#! /bin/sh

KERNEL_MAJOR_MINOR=$(uname -r | cut -c1-3)
echo $KERNEL_MAJOR_MINOR
down_dir="/tmp/cache/swt6621s-1.1.0"
mkdir -p  ${down_dir}
down_url="https://github.com/Novice-PG/swt6621s/archive/refs/tags/1.1.0.zip"
wget ${down_url} -O ${down_dir}/source.zip

unzip "${down_dir}/source.zip" -d /usr/src/
rm -rf "${down_dir}/source.zip"

# Build and install the kernel module via DKMS
 clear
 dkms remove -m swt6621s -v 1.1.0 
 dkms add -m swt6621s -v 1.1.0 && dkms build -m swt6621s -v 1.1.0 &&  dkms install -m swt6621s -v 1.1.0 ||  cat /var/lib/dkms/ea6621s/1.1.0/build/make.log
 
dkms add -m swt6621s -v 1.1.0
dkms build -m swt6621s -v 1.1.0 
dkms install -m swt6621s -v 1.1.0 

