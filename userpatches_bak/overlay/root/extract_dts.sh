#! /bin/sh
ls 
echo $PATH
echo $SHELL

mapfile --help

FS=' \t'
binwalk /dev/mmcblk1p2 | grep -i "Flattened device tree"  >  mmcblk1p2_files.txt 
mapfile  -t lines  < mmcblk1p2_files.txt
IFS=' '
while read -r line; do
echo $line
done      







for line in "${lines[@]}"; do
echo $line
read -ra words <<< $line
echo  Start:  ${words{0}}       Size:  ${words{6}}
#echo ${words{0}}
#echo ${words[6]}
done

echo ${words{0}}
echo ${words[1]}
echo ${words[2]}
echo ${words[3]}
