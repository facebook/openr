# `Spark`
---

`Spark` is the neighbor discovery module of Open/R. It leverages IPv6 link-local
multicast via UDP to discover and maintain Adjacencies, aka neighbor relationships.
The discovered neighbors, aka "Local Topology" of the node, is fed into the
system for KvStore database synchronization, and SPF Computation.

### Inter-Module Communications 
---

<img src="https://user-images.githubusercontent.com/51382140/90570487-a33ec300-e164-11ea-84ca-98485a646157.png" alt="Spark inside Open/R">

- [Producer]: `Spark` sends out neighbor event through `ReplicateQueue` to
`LinkMonitor`, which includs: UP/DOWN/RESTART/RTT-CHANGE events.

- [Consumer]: `Spark` receives interface database update through `RQueue` from
`LinkMonitor`. Neighbor discovery  will be applied on those interfaces ONLY.

> NOTE: For link UP/DOWN event, we expect lower level(i.e. `Netlink`) to notify
`LinkMonitor`. Further, it will notify `Spark` to start/stop sending out pkts.

### Spark Packet
---

`Spark` communicates to peer spark instance by broadcasting a UDP packet to
link-local multicast address `ff02::1`. The packet is sent over every configured
interface. The content of the packet evolves as neighboring spark instances
starts to learn about each other. 

The content of the packet is a serialized thrift object of type `SparkPacket`.
This internally consists of three main messages as its attributes. There will
be only one of them populated at a time to reduce control plane traffic.
- `SparkHelloMsg`
- `SparkHandshakeMsg`
- `SparkHeartbeatMsg`

> NOTE: By using thrift serialization/deserialization we completely avoid the
encoding and decoding complexity of data exchange. Thrift further provides a
good backward compatibility support as message structure evolves.

Check out [if/Spark.thrift](https://github.com/facebook/openr/blob/master/openr/if/Spark.thrift)
for detailed message structure.

High level speaking:

- `SparkHelloMsg` => Sent out periodically over all configured interfaces.
  It broadcasts discovered neighbor advertisements and future neighbor solicitation.
- `SparkHandshakeMsg` => Negotiation message to establish neighbor relationship, aka the `Adjacency`.
  Similar to TCP 3-way handshake process. Negotiation includes version, timers,
  and area configuration that we'll discuss below.
- `SparkHeartbeatMsg` => Send out peridodically for keep-alive purpose.

### Finite State Machine
---

<img src="https://user-images.githubusercontent.com/51382140/90571412-899e7b00-e166-11ea-97bd-419b493846cf.png" alt="Spark Neighbor State Transition Diagram">

`Spark` leverages Finite State Machine (FSM) to formulate neighbor state and
its transitions on event. FSM formulation ensures the correctness of state
handling and also greatly simplify the complexity of implementation.

```
SparkNeighState
- IDLE
- WARM
- NEGOTIATE
- ESTABLISHED
- RESTART

SparkNeighEvent
- HELLO_RCVD_INFO           => SparkHelloMsg received with node's self-info inside
- HELLO_RCVD_NO_INFO        => SparkHelloMsg received without node's self-info inside;
- HELLO_RCVD_RESTART        => neighbor going down and signal for GR;
- HEARTBEAT_RCVD            => keep alive msg received to refresh hold timer;
- HANDSHAKE_RCVD            => handshake acknowledgement;
- HEARTBEAT_TIMER_EXPIRE    => hold time expired;
- NEGOTIATE_TIMER_EXPIRE    => negotiate procedure timed out;
- GR_TIMER_EXPIRE           => graceful restart timer expired;
- NEGOTIATION_FAILURE       => negotiate procedure failed(e.g. area negotiation failure);
```

### State Transition Map
---

| EVENTs/STATEs           | IDLE  | WARM      | NEGOTIATE   | ESTABLISHED | RESTART     |
|-------------------------|-------|-----------|-------------|-------------|-------------|
| HELLO_RCVD_INFO         | WARM  | NEGOTIATE |             |             | ESTABLISHED |
| HELLO_RCVD_NO_INFO      | WARM  |           |             | IDLE        |             |
| HELLO_RCVD_RESTART      |       |           |             | RESTART     |             |
| HEARTBEAT_RCVD          |       |           |             | ESTABLISHED |             |
| HANDSHAKE_RCVD          |       |           | ESTABLISHED |             |             |
| HEARTBEAT_TIMER_EXPIRE  |       |           |             | IDLE        |             |
| NEGOTIATE_TIMER_EXPIRE  |       |           | WARM        |             |             |
| GR_TIMER_EXPIRE         |       |           |             |             | IDLE        |
| NEGOTIATION_FAILURE     |       |           | WARM        |             |             |

### SparkHelloMsg
---

- `SparkHelloMsg` contains node name, and the list of neighbors it has heard
from on this interface. This allows ALL neighbors on a segment to agree on
bidirectional visibility;
- Functionality:
  1) To advertise its own existence and basic neighbor information;
  2) To ask for immediate response for quick adjacency establishment;
  3) To notify for its own "RESTART" to neighbors;
- `SparkHelloMsg` is sent per interface;

### SparkHandshakeMsg
---

- `SparkHandshakeMsg` contains rest of necessary params to establish adjacency
with neighbor besides what has been learned from `SparkHelloMsg`;
- Functionality:
  1) Exchange `peerAddr` and `port` info for peer to establish TCP connection;
  2) areaId neogtiation;
  3) hold time and GR time negotiation;
- `SparkHandshakeMsg` is sent per (interface, neighbor) combination;

> NOTE: `SparkHandshakeMsg` has destination node attribute. Neighbors on the
same interface will ignore this message if it is NOT destined to itself.

### SparkHeartbeatMsg
---

- `SparkHeartbeatMsg` contains node name, sequence number;
- Functionality: notify its own aliveness
- `SparkHeartbeatMsg` is sent per interface;

### Timers
---

To maintain the state machine running smoothly, there are different kinds of timers used.
1. `helloTimer`: timer to control frequency of helloMsg. It is set **per ifName**;
2. `negotiateTimer`: timer to control frequency of handshakeMsg. It is set **per neighbor**;
3. `heartbeatTimer`: timer to control frequency of heartbeatMsg. It is set **per ifName**;
4. `heartbeatHoldTimer`: maximum hold time for neighbor adjacency. `SparkHeartbeatMsg`
will extend it;
5. `negotiateHoldTimer`: maximum time within `NEGOTIATE` state to avoid high volume of
negotiate packets being sent;
6. `gracefulRestartHoldTimer`: maximum time to hold neighbor adjacency under GR;

For typical configuration of above timer, please refer to `SparkConfig` section defined in
- [if/OpenrConfig.thrift](https://github.com/facebook/openr/blob/master/openr/if/OpenrConfig.thrift)

### Area Configuration
---

As area negotiation happens by default between spark instances, neighbor adjacency will ONLY
be formed if they can reach agreement on area.

For instance, nodeA and nodeB negotiates with area over `ethernet1`.

```
On nodeA:
AreaConfig = {
    area_id : "1",
    interface_regexes : ["ethernet1, port-channel.*"],
    neighbor_regexes : ["nodeB"]
}
```

```
On nodeB:
AreaConfig = {
    area_id : "1",
    interface_regexes : ["ethernet1, port-channel.*"],
    neighbor_regexes : ["nodeA"]
}
```
Both nodes will apply combination of `neighbor_regex`(i.e. regex for node_name) and
`interface_regex`(i.e. interface on which neighbor is discovered) to identify what area
neighbor should fall into. With above example, both nodeA and nodeB think neighbor should
be in area `1`. Hence the negotiation will go through.

> NOTE: negotiation failure will trigger state transition from `NEGOTIATE` to `WARM`
and stop sending `SparkHandshakeMsg`. See FSM transition part for deatils.

### RTT Measurement
---

With spark exchanging multicast packets for neighbor discovery we can easily
deduce the RTT between neighbors (reflection time). To reduce noise in
RTT measurements we use `Kernel Timestamps`. To avoid noisy `RTT_CHANGED` events
we use `StepDetector` so that small changes in RTT measurements are ignored.

### Fast Neighbor Discovery
---

When a node starts or a new link comes up, we perform fast initial neighbor
discovery by sending `SparkHelloMsg` with `solicitResponse` bit set. This is to 
request immediate reply, which allows quicker discovery of new neighbors(configurable).
