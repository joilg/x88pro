v26.8.0-trunk.121

testet with X88pro13 TV box  and Armbian build  branch v26.8.0-trunk.121

git clone --branch v26.8.0-trunk.121 https://github.com/armbian/build build
git clone  https://github.com/joilg/x88pro x88pro  
cp -R ./x88pro/userpatches build/
cd build/  
./compile.sh x88pro

Version 0.5

.