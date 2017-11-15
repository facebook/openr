# OpenR Library Examples
---------------------

One of OpenR's main offers is its modular design and extensibility. Here are a
few examples that illustrate how one can use the OpenR and fbzmq libraries to
write switch agents or centralized controllers that disseminate or query state
in the network. In fact, we use code very similar to these examples in
production networks here at Facebook.


### KvStoreAgent
---

This example shows how you may use KvStoreClient in another switch agent
running alongside OpenR to disseminate or learn state in the network.


### KvStorePoller
---

This is an example of how to extract KvStore contents from multiple OpenR
instances to gather info about your network.


### ZmqMonitorPoller
---

Similar to KvStorePoller, this example illustrates how to query counters and
collect logs from OpenR instances.
