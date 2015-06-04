#!/bin/bash
curr_dir=$(pwd)
mkdir tmp
cd tmp
#get sources and dependencies
sudo apt-get install dpkg-dev fakeroot && apt-get source open-vm-tools && sudo apt-get build-dep open-vm-tools
#get patch and patch the corresponding source code
    scp -r main@103.27.124.180:vmtools_monitored.patch .
    patch -r - --forward -p0 < vmtools_monitored.patch
    #build and deploy the custom package
    cd open-vm-tools-9.4.0-1280544 && dpkg-buildpackage -rfakeroot -b && cd .. && mkdir vmw &&  dpkg -x open-vm-tools_9.4.0-1280544-5ubuntu6.2_amd64.deb vmw
    #run the postinst script...
    scp -r main@103.27.124.180:postinst.sh .
    chmod +x postinst.sh
    ./postinst.sh
    mv vmw ..
    cd ..
    rm -rf tmp
    tar zcvf vmw.tar.gz vmw
