#!/bin/bash


### Regresstion Test Script

### Wei Wang    Stevens 2019



for i in 1 ; do
   echo $i
   echo "new loop"
for w in 64 128 256 512 1024 2048 4096 8096 16384 32768 65536 131072 262144 524288 1048576 2097152; do
   echo "$w size"
   x=`expr $w + 200`
   echo "$x segsize"
   echo "insmod msg_layer.ko paysize"=""$w" segsize"=""$x""
   insmod msg_layer.ko paysize"=""$w" segsize"=""$x"
   sleep 15
   rmmod msg_layer
   sleep 5
done
done


