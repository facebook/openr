# Platform

Open/R relies on `Platform` layer to provide the service to program routes(
i.e. Unicast Routes/MPLS Routes) on actual underneath system. `Platform`
must provide Thrift based service via certain port(e.g. 60100 by
default) to receive thrift client request for route programming. It is the
abstract layer, which can be implemented differently on individual system.

For example:
- `NetlinkProtocolSocket` to program route on Linux platform;
- Platform-specific agent on vendor device to fulfill SDK call;

> NOTE: link information retrieval and interface address programming is done
by directly interacting with `NetlinkProtocolSocket` via Linux Kernel Interface.

### Inter-Module Communications
---

<img alt="Platform Architecture" src="https://user-images.githubusercontent.com/51382140/90934844-e09a8f00-e3b6-11ea-9695-cb9836240118.png">

### Thrift APIs
---

#### Unicast Route APIs

```
/*
 * @params: clientId => identify client to program route
 *          route => single unicast route to be added
 * @return: None
 */
void addUnicastRoute(i16 clientId, Network.UnicastRoute route);

/*
 * @params: clientId => identify client to program route
 *          routes => collection of unicast routes to be added
 * @return: None
 */
void addUnicastRoutes(i16 clientId, list<Network.UnicastRoute> routes);

/*
 * @params: clientId => identify client to delete route
 *          prefix => single unicast prefix to be deleted
 * @return: None
 */
void deleteUnicastRoute(i16 clientId, Network.IpPrefix prefix);

/*
 * @params: clientId => identify client to delete route
 *          prefixes => collection of unicast prefixes to be deleted
 * @return: None
 */
void deleteUnicastRoutes(i16 clientId, list<Network.IpPrefix> prefixes);

/*
 * @params: clientId => identify client to sync route
 *          routes => collection of unicast routes to be synced.
 *                    This will flush ALL existing routes and install new one.
 * @return: None
 */
void syncFib(i16 clientId, list<Network.UnicastRoute> routes);

/*
 * @params: clientId => identify client to fetch route
 * @return: a vector of UnicastRoute programmed by this clientId
 */
list<Network.UnicastRoute> getRouteTableByClient(i16 clientId);
```

#### MPLS Route APIs

```
/*
 * @params: clientId => identify client to program route
 *          routes => collection of MPLS routes to be added
 * @return: None
 */
void addMplsRoutes(i16 clientId, list<Network.MplsRoute> routes);

/*
 * @params: clientId => identify client to delete route
 *          topLabels => collection of MPLS labels to be deleted
 * @return: None
 */
void deleteMplsRoutes(i16 clientId, list<i32> topLabels);

/*
 * @params: clientId => identify client to sync route
 *          routes => collection of MPLS routes to be synced. This
 *          will flush ALL existing routes and install new one.
 * @return: None
 */
void syncMplsFib(i16 clientId, list<Network.MplsRoute> routes);

/*
 * @params: clientId => identify client to fetch route
 * @return: a vector of MplsRoute programmed by this clientId
 */
list<Network.MplsRoute> getMplsRouteTableByClient(i16 clientId);
```

For more information, checkout
- [if/Platform.thrift](https://github.com/facebook/openr/blob/master/openr/if/Platform.thrift)


### Support on Linux Platform
---

`NetlinkFibHandler` provides thrift service interface for on-box client modules
(i.e. Fib) to program routes through Linux Kernel Interface. Client dispatches
thrift call to update routes or get full route table from Platform. Client can
periodically synchronize with service by keep alive check call, a re-sync request
is supported by the handler to re-send routing information upon client restart.


### Support on other Platform
---

To support platform other than Linux, developers should implement the thrift
service APIs in [if/Platform.thrift](https://github.com/facebook/openr/blob/master/openr/if/Platform.thrift)
