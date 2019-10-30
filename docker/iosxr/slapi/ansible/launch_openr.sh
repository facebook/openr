#!/bin/bash

set -x

if [[ $1 == "" ]]; then
   echo "No rtr name provided, bailing out..."
else
   rtr_name=$1
fi

route_batch_flag=$2


docker run -itd --restart=always --name openr --cap-add=SYS_ADMIN --cap-add=NET_ADMIN  -v /var/run/netns:/var/run/netns -v /misc/app_host/bash_trap.sh:/root/bash_trap.sh -v /misc/app_host/${rtr_name}:/root/${rtr_name} -v /misc/app_host/${rtr_name}/hosts:/etc/hosts --hostname $rtr_name akshshar/openr-xr /root/bash_trap.sh $rtr_name $route_batch_flag 
