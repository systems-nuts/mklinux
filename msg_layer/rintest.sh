#!/bin/bash


### Regresstion Test Script

### Wei Wang    Stevens 2019

for w in `seq 1 100`
 do
   insmod interrupt.ko 
   sleep 15
   rmmod interrupt
   sleep 5
   echo $w
done
