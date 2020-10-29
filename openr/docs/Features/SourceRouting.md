`Source Routing (SR)`
--------------------------


## Introduction
---

Source Routing is a forwarding mechanism in a network where a path for the packet
is determined at the source node. In contrast, in conventional routing, the
path for the packet is determined at every node based on its destination address.

Segment Routing, RFC [8402] leverages the Source Routing paradigm. Conceptually, a
packet entering the network gets tagged with forwarding instructions, termed as
segments. The tagging of segments can be carried out based on packet header such
as destination address, class of service, etc, and is termed as Forwarding
Equivalence Classes (FEC).

One or more ordered segments can be attached to the packet. Each segment encodes
special forwarding instruction. On subsequent nodes within network, the packet
is forwarded according to top segment instruction associated with the packet. As
per the instruction encoded in the segment, it can be altered or dropped, or
more segments can be pushed on the top.

At the last hop, all segments should've been dropped added by this network and
network will be forwarded as per its destination address.

### Segments
---

A Segment is an instruction that expresses the forwarding intent of the packet.
Open/R supports two types of segments described in RFC [8402]. `Adjacency`
segment and and `Node` segment.

Regardless of type, there is a scope associated with each segment, being `Area`
and `Local`. The scope determines the set of nodes that can understand and forward
the packet as per the associated segment.

#### Scope - Local
The segment's instruction can only be processed by the node that originates or
own the segment. The local segment may have undesirable behavior on the other
nodes.

#### Scope - Area
The segment instruction is well understood by all nodes within the area. Every
node can process and forward the packet associated with this segment.

#### Node Segment (Node SID)
Node Segment is a unique segment associated with every node in an area. The scope
of this segment is the `Area`.

Node Segments forwards the network packets to the associated node via
`Shortest Path` algorithm within an area. The segment instruction is retained
on the packet, until it reaches the destination node. Packet is then routed
according to the next segment or IP routing.

Penultimate NextHop Popping (PHP) is industry practice, where the last segment
is popped up before it is forwarded to the destination node. This helps in
saving one extra cycle of lookup on LER. Open/R performs PHP by default for Node
Segment.

#### Adjacency Segment (Adj SID)
Adjacency Segment is a unique segment associated with every Open/R adjacency.
The scope of this segment is `Local`.

Adjacency Segment forwards the network packets towards the associated adjacency
and pops the segment before doing so.

### Data Plane
---

Segment Routing is a concept and realizing its actual behavior can be achieved
through two data plane forwarding techniques, MPLS and SRv6.

MPLS segment is encoded as 4-byte integer and is prepended onto the IP packet.
Multiple segments can be added, each incurring 4-byte overhead. The packet is
looked up based on the top label into a special Label Forwarding Information
Base (L-FIB) in hardware and forwarded according to it. [Read more](https://tools.ietf.org/html/rfc3031)

SRv6 is pure IPv6 based forwarding. The extended header of IPv6 packet is
leveraged to encode segment information. Each segment is an IPv6 address and
incurs 16-byte of overhead. [Read more](https://tools.ietf.org/html/rfc8754)

In Open/R we chose MPLS as data-plane for forwarding mechanism for two reasons
- Widely adopted and supported by network hardware
- Incurs less overhead on the packet

### MPLS Segment Operations
---

There are 3 types of operations a network device needs to support to segment
routing with MPLS

#### Push
Add one or more labels on the top of the incoming packet (determined by FEC)
with egress instruction (nexthop/interface). This operation often happens on LER
when packet enters into SR enabled network.

#### Continue
Swapping the incoming label with one or more outgoing label(s) along with egress
instruction (next-hop/interface). This operation is often carried out to retain
label or swap label when packet is transitioning within SR enabled network.

For tunneling purpose or to address label stack depth limitation, we can also
swap an incoming label with an outgoing label-stack with an egress instruction
(i.e., binding SID).

#### Next
Popping the topmost label. Often carried out when the packet reaches or will
reach in immediate next-hop to the destination indicated by the topmost label.

### Configuration
---

#### Enabling Segment Routing
Configuration Knob in [OpenrConfig.thrift](../if/OpenrConfig.thrift#L211)
```
  13: optional bool enable_segment_routing
```

#### Configuring the Label Ranges
There are two ranges statically defined in the code as constants for Area and
Local segment scopes.

```
Area Range: [101, 49999]
Local Range: [50000, 59999]
```

#### Enabling IP->MPLS
Every route advertisement in Open/R allows the specification of forwarding
type. Setting this to `SR_MPLS` via config will make all Label Edge Routers (LER)
to programm `IP to MPLS` route for all advertisement from the node advertising
IP prefixes. The advertised prefix will assume the node label (aka Prefix SID).
This works with anycast prefixes as well.

Configuration for Forwarding Type [OpenrConfig.thrift](if/OpenrConfig.thrift#L210)
```
    12: PrefixForwardingAlgorithm prefix_forwarding_algorithm,
```

### Label Allocation
---

There are two label types that Open/R will perform per node. A node-label which
is unique within a whole network and per node adjacency-label which is unique
within the node and has local significance only.

* NodeSID will take node-label
* AdjacencySID will take adjacency-label

Open/R  supports allocating unique label/integer per node (LDP function). It is
implemented as a distributed computing enabled by KvStore. Also each adjacency
is assigned unique label on a node, usually derived from interface index.

You can look at the labels by inspecting adjacencies
```
[root@node1 ~]$ breeze kvstore adj

> node1 => Version: 54, *Node Label: 21890*
Neighbor  Local Intf  Remote Intf  Metric  *Label*  NextHop-v4   NextHop-v6                 Uptime
node2     if_1_2_1    if_2_1_1     1       *50006*  169.254.0.3  fe80::b4fe:aff:feb0:4141   3d21h
node33    if_1_33_1   if_33_1_1    1       *50007*  169.254.0.5  fe80::6821:64ff:fe7b:d06f  3d21h
```

### MPLS Programming APIs
---

Open/R will compute MPLS routes as per the Segment Routing architecture. This
will need to be programmed in HW. The [Platform](Platform.md) will need to provide
implementation for programming these routes in HW. Open/R provides the
implementation for programming these routes in MPLS enabled Linux kernel.

### References
---
* [Segment Routing Architecture](https://tools.ietf.org/html/rfc8402)
* [Multiprotocol Label Switching Architecture](https://tools.ietf.org/html/rfc3031)
* [IPv6 Segment Routing Header (SRH)](https://tools.ietf.org/html/rfc8754)
* [Segment Routing with the MPLS Data Plane](https://tools.ietf.org/html/rfc8660)
* [Intro to Segment Routing](https://www.cisco.com/c/en/us/td/docs/ios-xml/ios/seg_routing/configuration/xe-3s/segrt-xe-3s-book/intro-seg-routing.pdf)
* [Segment Routing in Linux Kernel](http://www.segment-routing.net/open-software/linux/)
* [MPLS Tutorial for Linux](https://netdevconf.info/1.1/proceedings/slides/prabhu-mpls-tutorial.pdf)
* [MPLS in GRE Tunnel on Linux](https://jsteward.moe/mpls-in-gre-tunnel-linux.html)
