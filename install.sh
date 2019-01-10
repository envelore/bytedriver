#!/bin/bash
# display user home
echo "Home for the current user is: $HOME"
make && sudo insmod bytedriver.ko sizeOfNewBuffer=1000 && sudo chmod 0777 /dev/envel && sudo dmesg -c
