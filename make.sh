#!/bin/sh
# 
# File:   make.sh
# Author: alexk
#
# Created on Sep 18, 2018, 12:48:12 AM
#
c++ -lcityhash -o ./build/npf_city_hasher.o -c ./npf_city_hasher.cpp
c++ -std=c++11 -I${RTE_SDK}/x86_64-native-linuxapp-gcc/include/ -o ./build/npf_conn_map.o -c ./npf_conn_map.cpp
gcc -std=c11 -I${RTE_SDK}/x86_64-native-linuxapp-gcc/include/ -o ./build/thmap.o -c ./thmap.c