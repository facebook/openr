The current solution is shown below:

![Open/R integration with IOS-XR- current design](/openr_xr_integration_current.png)



## [Docker Build](https://github.com/akshshar/openr-xr/tree/openr20171212/docker)
  
  Drop into the `docker` directory and issue the docker build command:
  
  **Tip:** When issuing the docker build command with this Dockerfile, make sure to use the --squash flag: `docker build --squash -it openr .`
  This is required to prevent the size of the docker image from spiraling out of control.

  ```
  cd docker/
  docker build --squash -it openr .

  ``` 
  This should build the docker image that contains integrated for :

  1) Open/R
  2) Open/R IosxrslFibHandler that uses gRPC to connect to IOS-XR service layer API and handles route downloads.
  3) Open/R xrtelemetry code that uses gRPC to receive a stream of IPv6 neighbors learnt over RP and linecards and programs the RP kernel where Open/R runs.  
    
  
### Deploying Open/R Docker image on NCS5500

IOS-XR utilizes a consistent approach towards the application hosting infrastructure across all XR platforms. This implies that all hardware platforms: 1RU, 2RU, Modular or even Virtual platforms would follow the same deployment technique described below:

In the demo, two NCS5501s are connected to each other over a HundredGig interface.
The basic configuration on each router is shown below:


**XR Configuration:**

```

interface HundredGigE0/0/1/0
 ipv4 address 10.1.1.10 255.255.255.0
 <mark>ipv6 nd unicast-ra</mark>
 ipv6 enable
!
!
!
grpc
 <mark>port 57777</mark>
 service-layer
!
!
telemetry model-driven
 sensor-group IPV6Neighbor
  sensor-path Cisco-IOS-XR-ipv6-nd-oper:ipv6-node-discovery/nodes/node/neighbor-interfaces/neighbor-interface/host-addresses/host-address
 !
 subscription IPV6
  sensor-group-id IPV6Neighbor sample-interval 15000
 !
!
end


```

Here, `ipv6 nd unicast-ra` is required to keep neighbors alive in XR while Open/R initiates traffic in the linux kernel.   
The `grpc` configuration starts the gRPC server on XR and can be used to subscribe to Telemetry data (subscription IPv6 as shown in the configuration above)   
and service-layer configuration allows Service-Layer clients to connect over the same gRPC port.



Once the Docker image is ready, set up a private docker registry that is reachable from the NCS5500 router in question and push the docker image to that registry. Setting up a private docker registry and pulling a docker image onto NCS5500 is explained in detail in the "Docker on XR" tutorial here:  <https://xrdocs.github.io/application-hosting/tutorials/2017-02-26-running-docker-containers-on-ios-xr-6-1-2/#private-insecure-registry>{:target="_blank"} 

Once the docker image is pulled successfully, you should see:

```
RP/0/RP0/CPU0:rtr1#bash
Fri Feb 16 22:46:52.944 UTC
[rtr1:~]$ 
[rtr1:~]$ [rtr1:~]$ docker images
REPOSITORY              TAG                 IMAGE ID            CREATED             SIZE
11.11.11.2:5000/openr   latest              fdddb43d9600        33 seconds ago        1.829 GB
[rtr1:~]$ 
[rtr1:~]$ 
```

Now, simply spin up the docker image using the parameters shown below:

```

RP/0/RP0/CPU0:rtr1#
RP/0/RP0/CPU0:rtr1#bash
Fri Feb 16 22:46:52.944 UTC
[rtr1:~]$ 
[rtr1:~]$ docker run -itd  --name openr --cap-add=SYS_ADMIN --cap-add=NET_ADMIN  -v /var/run/netns:/var/run/netns -v /misc/app_host:/root -v /misc/app_host/hosts_rtr1:/etc/hosts --hostname rtr1 11.11.11.2:5000/openr bash
684ad446ccef5b0f3d04bfa4705cab2117fc60f266cf0536476eb9506eb3050a
[rtr1:~]$ 
[rtr1:~]$ 
[rtr1:~]$ docker ps
CONTAINER ID        IMAGE                   COMMAND             CREATED             STATUS              PORTS               NAMES
b71b65238fe2        11.11.11.2:5000/openr   "bash -l"           24 secondss ago        Up 24 seconds                             openr
```

Instead of `bash` as the entrypoint command for the docker instance, one can directly start openr using `/root/run_openr_rtr1.sh > /root/openr_logs 2>&1`. I'm using `bash` here for demonstration purposes.

Note the capabilities: `--cap-add=SYS_ADMIN` and `--cap-add=NET_ADMIN`. Both of these are necessary to ensure changing into a mounted network namespace(vrf) is possible inside the container.



Once the docker instance is up on rtr1, we do the same thing on rtr2:

```
RP/0/RP0/CPU0:rtr2#bash
Fri Feb 16 23:12:53.828 UTC
[rtr2:~]$ docker ps
CONTAINER ID        IMAGE                   COMMAND             CREATED             STATUS              PORTS               NAMES
684ad446ccef        11.11.11.2:5000/openr   "bash -l"           8 minutes ago       Up 8 minutes                            openr
[rtr2:~]$ 
```

On rtr2, the file `/root/run_openr_rtr2.sh` is slightly different. It leverages `increment_ipv4_prefix.py` as a route-scaling script to increase the number of routes advertized by rtr2 to rtr1. Here I'll push a 1000 routes from rtr2 to rtr1 to test the amount of time Open/R on rtr1 takes to program XR RIB.


### Testing FIB Programming Rate

On rtr2, exec into the docker instance and start Open/R:

```
RP/0/RP0/CPU0:rtr2#bash
Fri Feb 16 23:12:53.828 UTC
[rtr2:~]$ 
[rtr2:~]$ docker exec -it openr bash
root@rtr2:/# /root/run_openr_rtr2.sh
/root/run_openr_rtr2.sh: line 106: /etc/sysconfig/openr: No such file or directory
Configuration not found at /etc/sysconfig/openr. Using default configuration
openr[13]: Starting OpenR daemon.
openr

....

```

Now, hop over to rtr1 and do the same:

```

RP/0/RP0/CPU0:rtr1#bash
Fri Feb 16 23:12:53.828 UTC
[rtr1:~]$ 
[rtr1:~]$ docker exec -it openr bash
root@rtr2:/# /root/run_openr_rtr1.sh
/root/run_openr_rtr1.sh: line 106: /etc/sysconfig/openr: No such file or directory
Configuration not found at /etc/sysconfig/openr. Using default configuration
openr[13]: Starting OpenR daemon.
openr

I0216 23:50:28.058964   134 Fib.cpp:144] Fib: publication received ...
I0216 23:50:28.063819   134 Fib.cpp:218] <mark>Processing route database ... 1002 entries</mark>
I0216 23:50:28.065533   134 Fib.cpp:371] Syncing latest routeDb with fib-agent ... 
I0216 23:50:28.081434   126 IosxrslFibHandler.cpp:185] Syncing FIB with provided routes. Client: OPENR
I0216 23:50:28.105329    95 ServiceLayerRoute.cpp:197] ###########################
I0216 23:50:28.105350    95 ServiceLayerRoute.cpp:198] Transmitted message: IOSXR-SL Routev4 Oper: SL_OBJOP_UPDATE
VrfName: "default"
Routes {
  Prefix: 1006698753
  PrefixLen: 32
  RouteCommon {
    AdminDistance: 99
  }
  PathList {
    NexthopAddress {
      V4Address: 167837972
    }
    NexthopInterface {
      Name: "HundredGigE0/0/1/0"
    }
  }
}
Routes {



.....




Routes {
  Prefix: 1677976064
  PrefixLen: 24
  RouteCommon {
    AdminDistance: 99
  }
  PathList {
    NexthopAddress {
      V4Address: 167837972
    }
    NexthopInterface {
      Name: "HundredGigE0/0
I0216 23:49:00.923399    95 ServiceLayerRoute.cpp:199] ###########################
I0216 23:49:00.943408    95 ServiceLayerRoute.cpp:211] RPC call was successful, checking response...
I0216 23:49:00.943434    95 ServiceLayerRoute.cpp:217] IPv4 Route Operation:2 Successful
I0216 23:49:00.944533    95 ServiceLayerRoute.cpp:777] ###########################
I0216 23:49:00.944545    95 ServiceLayerRoute.cpp:778] Transmitted message: IOSXR-SL RouteV6 Oper: SL_OBJOP_UPDATE
VrfName: "default"
I0216 23:49:00.944550    95 ServiceLayerRoute.cpp:779] ###########################
I0216 23:49:00.945046    95 ServiceLayerRoute.cpp:793] RPC call was successful, checking response...
I0216 23:49:00.945063    95 ServiceLayerRoute.cpp:799] IPv6 Route Operation:2 Successful
I0216 23:27:01.021437    52 Fib.cpp:534] OpenR convergence performance. Duration=3816
I0216 23:27:01.021456    52 Fib.cpp:537]   node: rtr1, event: ADJ_DB_UPDATED, duration: 0ms, unix-timestamp: 1518823617205
I0216 23:27:01.021464    52 Fib.cpp:537]   node: rtr1, event: DECISION_RECEIVED, duration: 1ms, unix-timestamp: 1518823617206
I0216 23:27:01.021471    52 Fib.cpp:537]   node: rtr1, event: DECISION_DEBOUNCE, duration: 9ms, unix-timestamp: 1518823617215
I0216 23:27:01.021476    52 Fib.cpp:537]   node: rtr1, event: DECISION_SPF, duration: 22ms, unix-timestamp: 1518823617237
I0216 23:27:01.021479    52 Fib.cpp:537]   node: rtr1, event: FIB_ROUTE_DB_RECVD, duration: 12ms, unix-timestamp: 1518823617249
I0216 23:27:01.021484    52 Fib.cpp:537]   node: rtr1, event: FIB_DEBOUNCE, duration: 3709ms, unix-timestamp: 1518823620958
I0216 23:27:01.021488    52 Fib.cpp:537]   node: rtr1, event: <mark>OPENR_FIB_ROUTES_PROGRAMMED, duration: 63ms, unix-timestamp: 1518823621021</mark>


```

The above logs show how the code programs service layer routes into IOS-XR RIB.
Here are breeze (open/R cli) outputs on rtr1 and rtr2:

**rtr1 breeze adj**:

```

RP/0/RP0/CPU0:rtr1#bash
Sat Feb 17 00:18:07.974 UTC
[rtr1:~]$ 
[rtr1:~]$ docker exec -it openr bash
root@rtr1:/# 
root@rtr1:/# ip netns exec global-vrf bash
root@rtr1:/# 
root@rtr1:/# 
root@rtr1:/# breeze kvstore adj

&gt; rtr1's adjacencies, version: 2, Node Label: 36247, Overloaded?: False
Neighbor    Local Interface    Remote Interface      Metric    Weight    Adj Label  NextHop-v4    NextHop-v6                Uptime
<mark>rtr2        Hg0_0_1_0          Hg0_0_1_0                 10         1        50066  10.1.1.20     fe80::28a:96ff:fec0:bcc0  1m36s</mark>


root@rtr1:/# 

```

### Check IOS-XR RIB State

Open/R  instances were able to run inside docker containers on two back-to-back NCS5501 devices, connected over a HundredGig port. 
Check rtr1's RIB since we injected 1000 routes into this open/R instance:

```

RP/0/RP0/CPU0:rtr1#show route
Sat Feb 17 00:28:36.838 UTC

Codes: C - connected, S - static, R - RIP, B - BGP, (>) - Diversion path
       D - EIGRP, EX - EIGRP external, O - OSPF, IA - OSPF inter area
       N1 - OSPF NSSA external type 1, N2 - OSPF NSSA external type 2
       E1 - OSPF external type 1, E2 - OSPF external type 2, E - EGP
       i - ISIS, L1 - IS-IS level-1, L2 - IS-IS level-2
       ia - IS-IS inter area, su - IS-IS summary null, * - candidate default
       U - per-user static route, o - ODR, L - local, G  - DAGR, l - LISP
       A - access/subscriber, a - Application route
       M - mobile route, r - RPL, (!) - FRR Backup path

Gateway of last resort is 10.1.1.20 to network 0.0.0.0

S*   0.0.0.0/0 [1/0] via 10.1.1.20, 1d02h
               [1/0] via 11.11.11.2, 1d02h
C    10.1.1.0/24 is directly connected, 3w0d, HundredGigE0/0/1/0
L    10.1.1.10/32 is directly connected, 3w0d, HundredGigE0/0/1/0
L    10.10.10.10/32 is directly connected, 3w0d, Loopback1
C    11.11.11.0/24 is directly connected, 1d02h, MgmtEth0/RP0/CPU0/0
L    11.11.11.23/32 is directly connected, 1d02h, MgmtEth0/RP0/CPU0/0
L    50.1.1.1/32 is directly connected, 3w0d, Loopback0
a    60.1.1.1/32 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.1.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.2.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.3.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.4.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.5.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.6.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.7.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.8.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.9.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.10.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.11.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.12.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.13.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0
a    100.1.14.0/24 [99/0] via 10.1.1.20, 00:03:14, HundredGigE0/0/1/0

....


RP/0/RP0/CPU0:rtr1#show route summary
Sat Feb 17 00:29:47.961 UTC
Route Source                     Routes     Backup     Deleted     Memory(bytes)
local                            4          0          0           960          
connected                        2          2          0           960          
dagr                             0          0          0           0            
static                           1          0          0           352          
bgp 65000                        0          0          0           0            
application Service-layer        1002       0          0           240480       
Total                            1009       2          0           242752       

RP/0/RP0/CPU0:rtr1#

```

There you go! There are 1002 service layer routes in the RIB, all thanks to Open/R acting as an IGP, learning routes from its neighbor and programming the IOS-XR RIB on the local box over gRPC.

