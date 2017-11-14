`Spark`
-------

`Spark` is a simple hello protocol we use for neighbor discovery. You can find
packets definitions in if/Spark.thrift. The Hello packet contains the node name,
its Curve ECC public key, and list of neighbors it has heard from . This allows
all neighbors on a segment to agree on bidirectional visibility. Once we see
ourselves in neighbor's hello packets we inform the downstream consumer of a
neighbor event. Each packet also carries the hold time, which tells the receiver
how long to keep the information valid for. There is no provision for p2p
sessions or rapid hellos - we expect the lower layers to tell us of interface
event quickly.

The rate of hello packet send is defined by `keepAliveTime` and this time must
be less than the `holdTime` for each node. Having these times as separate
makes things easy for Graceful Restart. `holdTime` sort of covers the concept
of `restartTime` as well. Adjacency with a neighbor can be established in
`2 * keepAliveTime` in worst case as per current implementation.

### APIs
---

Spark listen on `LinkMonitor` PUB channel for link events and appropriately
start/stop neighbor discovery on the node. Spark uses single UDP socket that
joins multicast group `ff02::1` on all interfaces it has been configured to
listen on. When receiving packets, we peek the interface index the packet was
received on. When sending packet out, we specify the interface index to send
packet out on. That's it, nothing much else.

For more information about message formats, checkout
- [if/Spark.thrift](https://github.com/facebook/openr/blob/master/openr/if/Spark.thrift)

### Encrypted Packets
---

Every hello packet can optionally be signed by the sender's private key, and
verify signature on reception. The public key in the packet is reported in
neighbor event, and allows the downstream consumer to implement authentication,
e.g. compare it to the list of known keys.

### RTT Measurement
---

With spark exchanging multicast packets for neighbor discovery we can easily
deduce the RTT between neighbors (reflection time). To reduce noise in
RTT measurements we use `Kernel Timestamps`. Further to avoid correctly identify
the `RTT_CHANGED` event we use `StepDetector` so that small changes in RTT
measurements are ignored.

### Fast Neighbor Discovery
---

When node starts or new link comes up we perform fast initial neighbor discovery
by sending hello packets with `solicitResponse` bit set to request for immediate
reply. This allows us to discover new neighbors in `~100ms` (configurable).
