`Spark`
-------

`Spark` is a simple hello protocol we use for neighbor discovery. You can find
packet definitions in if/Spark.thrift. The Hello packet contains the node name,
and the list of neighbors it has heard from. This allows all neighbors on a
segment to agree on bidirectional visibility. Once we see ourselves in a
neighbor's hello packets we inform the downstream consumer of a neighbor event.
Each packet also carries the hold time, which tells the receiver how long to
keep the information valid for. There is no provision for p2p sessions or rapid
hellos - we expect the lower layers to tell us of interface event quickly.

The rate of hello packet send is defined by `keepAliveTime` and this time must
be less than the `holdTime` for each node. Having these times as separate
makes things easy for Graceful Restart. `holdTime` sort of covers the concept
of `restartTime` as well. An adjacency with a neighbor can be established in
`2 * keepAliveTime` in the worst case under the current implementation.

### APIs
---

Spark listens on `LinkMonitor` PUB channel for link events and appropriately
starts/stops neighbor discovery on the link. Spark uses a single UDP socket that
joins multicast group `ff02::1` on all interfaces it has been configured to
listen on. When receiving packets, we peek the interface index the packet was
received on. When sending a packet out, we specify the interface index to send
the packet out on. That's it, nothing much else.

For more information about message formats, check out
- [if/Spark.thrift](https://github.com/facebook/openr/blob/master/openr/if/Spark.thrift)

### RTT Measurement
---

With spark exchanging multicast packets for neighbor discovery we can easily
deduce the RTT between neighbors (reflection time). To reduce noise in
RTT measurements we use `Kernel Timestamps`. To avoid noisy `RTT_CHANGED` events
we use `StepDetector` so that small changes in RTT measurements are ignored.

### Fast Neighbor Discovery
---

When a node starts or a new link comes up we perform fast initial neighbor
discovery by sending hello packets with `solicitResponse` bit set to request an
immediate reply. This allows us to discover new neighbors in `~100ms` (configurable).
