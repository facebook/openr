# Monitor - Process telemetry and events

## Introduction

---

`Monitor` is responsible for calculating and exporting the counters of the
Open/R system metrics, as well as processing the Open/R event logs injected from
other individual modules.

## Inter Module Communication

---

`LogSampleQueue` is the channel for event log collection. Each module logs the
important event, packs it as a LogSample object and pushes it into the queue.

- [Producer `KvStore`] events including:
  - `KVSTORE_FULL_SYNC`
  - `KEY_EXPIRE`
- [Producer `Fib`]:
  - `ROUTE_CONVERGENCE`
- [Producer `LinkMonitor`]:
  - `ADD_PEER`
  - `DEL_PEER`
  - All
    [SparkNeighborEventType](https://github.com/facebook/openr/blob/master/openr/if/Spark.thrift):
    - `NEIGHBOR_UP`
    - `NEIGHBOR_DOWN`
    - `NEIGHBOR_RESTARTED`
    - `NEIGHBOR_RTT_CHANGE`
    - `NEIGHBOR_RESTARTING`
- [Producer `PrefixAllocator`]:
  - `ALLOC_PARAMS_UPDATE`
  - `PREFIX_ELECTED`
  - `PREFIX_ELECTED`
  - `PREFIX_LOST`
- [Consumer `RQueue<LogSample>`]: Read `LogSample` object and process all event
  logs.

## Deep Dive

---

High-level speaking, the instance of
[Monitor](https://github.com/facebook/openr/blob/master/openr/monitor/Monitor.cpp)
runs as an independent thread, which inherits from
[MonitorBase](https://github.com/facebook/openr/blob/master/openr/monitor/MonitorBase.cpp).
They mainly provide the following functions:

- Calculate and update the Open/R process metric counters, including:

  1. Open/R process uptime (in second);
  2. Resident set size (RSS) of memory that Open/R process used (in Byte);
  3. CPU percentage the Open/R process currently used;

  - Details about how to calculate RSS and CPU%:
    [SystemMetrics](https://github.com/facebook/openr/blob/master/openr/monitor/SystemMetrics.cpp)

- Start a fiber to consume the output of `RQueue<LogSample>` to export logs
  injected by other Open/R modules
  - [LogSample](https://github.com/facebook/openr/blob/master/openr/monitor/LogSample.h)
    represents the loosely structured log event in the queue. Each log event is
    a JSON sample described as a dictionary.
  - We provide flexibility for your own logging storage solution. You could
    modify the code in
    [Monitor.cpp](https://github.com/facebook/openr/blob/master/openr/monitor/Monitor.cpp)
    to export the event logs to your database.
  - It has a toggle `enable_event_log_submission` in
    [OpenrConfig.thrift](https://github.com/facebook/openr/blob/master/openr/if/OpenrConfig.thrift)
    to enable or disable the log processing:

```
struct MonitorConfig {
  ...
  2: bool enable_event_log_submission  = false # disable the log processing
}
```

- Store and retrieve the most recent event logs:
  - It maintains a list of the most recent event logs in memory. Command
    `breeze monitor logs` could retrieve this list of recent logs.
  - The size of this list is configurable in
    [OpenrConfig.thrift](https://github.com/facebook/openr/blob/master/openr/if/OpenrConfig.thrift):

```
struct MonitorConfig {
  1: i32 max_event_log = 500 # Save the most recent 500 event logs
  ...
}
```
