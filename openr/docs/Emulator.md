`Emulator` coming up soon
----------

Emulation facilitates the testing of `OpenR` on large virtual network
topologies in order of minutes. This testing is very critical for any software
changes that gets pushed into repository and we enforce testing on at-least
`1000` (One Thousand) nodes topology before a code is checked in.

### Overview and Terminology
---

- `Host` => Physical machine on which nodes will be emulated
- `Node` => Virtual entity mimicking machine. We use container technique
  which provides network, process, disk isolation
- `Link` => Connects two virtual node with certain properties like loss,
  latency, jitter etc.
- `Topology` => Particular arrangement of nodes and links (e.g. grid, random)
  describes virtual topology being emulated.

`emulator` python tool is the main magic here. To get started with just do

```
emulator --help
```

#### How Node is Emulated ?

Each node is emulated via linux container (systemd-nspawn) which provides us
processes, network, disk and hostname isolation. It mimics a separate physical
machine.

Each node gets a unique `/64` out of rack's network space and it's NDP requests
are proxied by the host on which node is present. In this way we can add lot of
virtual nodes on host (as IP space is pretty big) without any changes in the
network for routing (normal routing/switching will be able to get packet
destined to virtual node correctly).

#### How Link is Emulated ?

Link is a pipe/circuit between two nodes with one interface being on each side.
In `config.json` each link is `directional` describing interface on particular
side of node. The Link is emulated using `gre tunnels` between two virtual
namespaces. There can be multiple links between two given nodes and we solve
it by using different keys for tunnels.

### Emulation Resources
---

Virtual topology is built on the top of set of physical resources and can span
over multiple physical servers. There are some assumptions which make Emulation
span across multiple hosts as described below.
- Each server has globally addressable IPv6 address and `/64` net-mask
- `<server-network>::feed::0/96` is not being used by anyone within the rack
- All emulation hosts are directly reachable from each other

### Functionalities
---

- Generate a user defined topology config (e.g grid, random, full-mesh)
- Setup all nodes, links and network configurations from the generated configs
- Perform various validations such as topology, fib, connectivity
- Perform real scenario behaviors such as link-flaps (bring up/down links),
  processes-flaps (restart some of openr processes)
- Other useful features to help test openr such as check hosts/nodes/services
  status, ssh to nodes etc.
