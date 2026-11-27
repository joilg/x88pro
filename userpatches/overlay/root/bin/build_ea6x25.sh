#! /bin/sh

KERNEL_MAJOR_MINOR=$(uname -r | cut -c1-3)
echo $KERNEL_MAJOR_MINOR
down_dir="/tmp/cache/ea6x21-dkms-${KERNEL_MAJOR_MINOR}"
mkdir -p  ${down_dir}
down_url="https://github.com/joilg/ea6x21-dkms/archive/refs/heads/${KERNEL_MAJOR_MINOR}.zip"
wget ${down_url} -O ${down_dir}/source.zip

cp "${down_dir}/source.zip" "/tmp/source.zip"
unzip /tmp/source.zip -d /usr/src/
rm -rf /tmp/ea6x21-main /tmp/source.zip

# Build and install the kernel module via DKMS
dkms add -m ea6x21-dkms -v ${KERNEL_MAJOR_MINOR}
dkms build -m ea6x21-dkms -v ${KERNEL_MAJOR_MINOR} 
dkms install -m ea6x21-dkms -v ${KERNEL_MAJOR_MINOR} 

