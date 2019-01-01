#!/bin/bash

docker run -itd --restart=always --name openr --cap-add=SYS_ADMIN --cap-add=NET_ADMIN  -v /var/run/netns:/var/run/netns -v /misc/app_host:/root -v /misc/app_host/hosts_rtr1:/etc/hosts --hostname rtr1 11.11.11.2:5000/openr bash -c "/root/run_openr_rtr1.sh > /root/openr.log 2>&1"
