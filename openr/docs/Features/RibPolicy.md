# RIB Policy

Routing Information Base (RIB) Policy provides ability to police the computed
routes before programming in underlying hardware. Currently supported use-case
is to modify `weight` parameter for each next-hop. However, it can be extended
in future to support changing `admin-distance` (aka route priority), add or drop
next-hops, change route metric, and block route from programming or
re-distribution.

## Flow Diagram

---

`Decision` module in Open/R implements the RIB Policy processing. The RIB Policy
applied on computed routes just before they're sent off for programming.

![Decision Flow](https://user-images.githubusercontent.com/1482609/84713146-3f5b0c80-af1f-11ea-8d6b-58d7ce7a6a90.png)

## Creating RIB Policy

---

RIB Policy is expressed in thrift specification and is defined as
`struct RibPolicy` in
[`OpenrCtrl.thrift`](https://github.com/facebook/openr/blob/master/openr/if/OpenrCtrl.thrift).
Please refer to the documentation associated with the defined structs for
details.

At a high level RIB Policy consists of two parts, Matcher (Selection) and Action
(Transformation). The matcher specified criteria for route selection on which
action is to be performed. Action specifies the transformation intent. One such
pair of Match-Action is termed as `RibPolicyStatement`. Multiple such statements
can be specified. However, note that only first matching action will be applied.

## Setting RIB Policy

---

RIB Policy can be set via RPC API (aka thrift API). It is not supported to be
set via configuration as of now. Following two APIs provide set and get of
RibPolicy. It is important to note that policy have `ttl_secs` field which
specifies the validity of policy, beyond which it is expired and effects of
policy will be reverted.

```c++
// Set policy API
void setRibPolicy(1: RibPolicy ribPolicy) throws (1: OpenrError error)

// Get policy API
RibPolicy getRibPolicy() throws (1: OpenrError error)
```

[SetRibPolicy](https://github.com/facebook/openr/blob/master/examples/SetRibPolicyExample.cpp)
example code is provided for reference in terms of how to define and set
RibPolicy.

For more details refer to `OpenrCtrl` service interface in
[`OpenrCtrl.thrift`](https://github.com/facebook/openr/blob/master/openr/if/OpenrCtrl.thrift).

## UseCase - NextHop Weight Transformation (UCMP)

---

Define and achieve policy driven weigted distribution of flows over multiple
next-hops. This could be useful in context where correctness of forwarding state
(aka routes) comes from the distributed protocols, but the operator wants to
influence the traffic flows as per congesion in the network (e.g. send more to
less congested part of network).

RIB Policy allows operators to customize the weight for computed next-hops. As
of now the weights can be customized per `area` or per `neighbor` fields. The
`neighbor` weight takes precedence over the `area` weight. See
`RibRouteActionWeight` struct in
[`OpenrCtrl.thrift`](https://github.com/facebook/openr/blob/master/openr/if/OpenrCtrl.thrift)
for more information.

The next-hops that are processed through RIB Policy and gets assigned `weight=0`
will be removed. If policed route have no next-hop then it will be removed from
RIB. This won't be programmed as well won't get re-distributed across the areas.

### Configuration Knob

---

RibPolicy is by default disabled and RPC APIs will throw exception if an
application tries to set the policy. This is a safety mechanism to disable this
feature where it is not needed. You'll need to set the `enable_rib_policy` field
in OpenrConfig to `true` to make use of RibPolicy feature.

### CLI

---

Breeze CLI provides a command to view the currently configured RibPolicy.

```console
$ breeze decision rib-policy
> RibPolicy
  Validity: 291s
  Statement:
    Prefix Match List: ::/0
    Action Set Weight: default=0, area-weights={'0': 1}
```
