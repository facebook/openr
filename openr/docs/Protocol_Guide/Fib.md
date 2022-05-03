# Fib - Route Programming

## Introduction

---

`Fib` is responsible for programming the actual forwarding tables in hardware on
the local node. Open/R defines the RPC interface, which is the `Thrift Service`
defined inside `Platform.thrift`. Underlying platform is expected to implement
this. All forwarding table entries are programmed via this thrift interface.

## Inter Module Communication

---

![Fib Intermodule Communication](https://user-images.githubusercontent.com/51382140/103041020-0208b480-452a-11eb-97f0-bfe4016b3d99.png)

- `[Producer] ReplicateQueue<thrift::RouteDatabaseDelta>`: stream `routeDbDelta`
  to subscribers who want to receive updates for routes to be programmed.
- `[Consumer] RQueue<thrift::DecisionRouteUpdate>`: receive real-time updates
  from `Decision` and program update via thrift client call.

## Operations

---

For **Data Types and Format**, check out

- [if/Types.thrift](https://github.com/facebook/openr/blob/master/openr/if/Types.thrift)

For **Thrift Interface**, check out

- [if/Platform.thrift](https://github.com/facebook/openr/blob/master/openr/if/Platform.thrift)

## Deep Dive

---

`Fib` programs routes via external FibAgent (HW specific, implements
`FibService`) over thrift. Open/R offers `FibService` implementation for native
routing with Linux. The binary is named as `platform_linux` and code is located
at `openr/platform/` directory. For more detals, see [Platform.md](Platform.md)

Thrift port to communicate with underlying platform can be configured via
`fib_port` inside
[if/OpenrConfig.thrift](https://github.com/facebook/openr/blob/master/openr/if/OpenrConfig.thrift)
