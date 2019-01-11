
## Files meant to be mounted into the container:
Place the following files under `/misc/app_host` on the router:

*  **hosts_rtr2**: This is the /etc/hosts file meant for the docker instance and will contain the hosts definition based on the hostname of the router, in this example `rtr2`. To understand how it gets mounted into the system, check out `launch_openr.sh` in the same folder.
*  **run_openr_rtr2.sh**: This is the basic configuration file used by the open/R binary inside the docker instance. Keeping it mounted from the `/misc/app_host` folder into the docker instance, allows the Open/R configuration to be malleable without having to rebuild the docker image. Again, to understand how it gets mounted into the system, check out `launch_openr.sh` in the same folder.
*  **increment_ipv4_prefix2.py**: This is completely optional, but allows a quick way to advertise up to 1000 routes through the open/R instance to its neighbors by simply looping through a set of IPv4 addresses. To understand how it is utilized, grep for `increment_ipv4_prefix2.py` in `run_openr_rtr2.sh` and look at `launch_openr.sh` to understand how to mount the file into the docker container.

## Launch script

The `launch_openr.sh` script is a sample script to help launch the docker container for Open/R properly.
Checking its contents, we find the following settings:

```
docker run -itd \             >>> Allow interactive shells to be opened up into the container and daemonize it
           --restart=always \ >>> Make sure container is relaunched for any reload, power cycle event
           --name openr \   >>> Any custom name of choice
           --cap-add=SYS_ADMIN \   >>> Enable the scripts and binaries inside container to invoke setns and other utilities.
           --cap-add=NET_ADMIN \   >>> Enable the binaries and scripts inside to manipulate the kernel routing capabilities.
           -v /var/run/netns:/var/run/netns \   >>> Mount the IOS-XR vrf mapped network namespaces to alunch open/R in netns of choice
           -v /misc/app_host:/root \     >>>  Mount /misc/app_host and all relevant files to /root of container (or any folder of choice)
           -v /misc/app_host/hosts_rtr2:/etc/hosts \  >>> Mount hosts_<> file from /misc/app_host into /etc/hosts for Open/R hellos
           --hostname rtr2 \         >>>  Specify the same hostname that you set up in the hosts file for Open/R
           akshshar/openr-xr \   >>  Specify the name of the pulled Open/R image.
           bash -c "/root/run_openr_rtr2.sh > /root/openr.log 2>&1"   >>> Launch the Open/R binary using the run_openr_<> script and redirect logs to /root (and therefore to /misc/app_host on XR based on the earlier mount)

```
