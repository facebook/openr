#!/bin/bash

docker run -itd --restart=always --name openr --cap-add=SYS_ADMIN --cap-add=NET_ADMIN  -v /var/run/netns:/var/run/netns -v /misc/app_host:/root -v /misc/app_host/hosts_rtr1:/etc/hosts --hostname $1 akshshar/openr-xr /root/bash_trap.sh $1 $2 
