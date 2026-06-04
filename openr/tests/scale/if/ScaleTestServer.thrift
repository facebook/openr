/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package "meta.com/openr/tests/scale"

include "thrift/annotation/thrift.thrift"

namespace cpp2 openr.thrift
namespace py3 openr.tests.scale

struct DutConnection {
  1: optional string host;
  2: i32 port = 2018;
}

enum DutRole {
  SPINE = 1,
  LEAF = 2,
}

struct TopologyConfig {
  1: string type = "bbf-simple"; // "bbf-simple" | "bbf-full"
  2: optional DutRole dutRole;
  3: optional i32 numSpines;
  4: optional i32 numLeaves;
  5: optional i32 numSuperSpines;
  6: optional i32 numPods;
  7: optional i32 numSites;
  8: optional i32 numPrefixesPerNode;
  9: optional i32 ecmpWidth;
}

struct InjectionConfig {
  1: optional bool injectTopology;
  2: optional bool simulateNeighbors;
  3: optional bool enableFakeKvStore;
  // Unset = daemon allocates from its own pool (see --fake_kvstore_base_port).
  4: optional i32 fakeKvStoreBasePort;
  // Unset = disabled (no extra fake keys injected per node).
  5: optional i32 numFakeKeysPerNode;
  6: optional i32 fakeKeyVersionBumpIntervalSec;
  // Required when simulateNeighbors is true.
  7: optional list<string> interfaces;
}

struct ScaleTestConfig {
  1: DutConnection dut;
  2: TopologyConfig topology;
  3: InjectionConfig injection;
}

struct LinkRef {
  1: string localNode;
  2: string remoteNode;
}

// One simulated Spark neighbor's status.
struct SparkNeighborState {
  1: string name;
  2: string state; // SparkNeighState enum name (e.g. "ESTABLISHED")
  3: string dutNode; // DUT node name learned from the DUT's packets
  4: bool failed; // true if the neighbor was failed via downNode/downLink
}

// Aggregate SparkFaker packet stats plus the per-neighbor table. All counters
// are zero and neighbors empty when simulateNeighbors=false for the session.
struct NeighborStats {
  1: i64 hellosSent;
  2: i64 hellosReceived;
  3: i64 handshakesSent;
  4: i64 handshakesReceived;
  5: i64 heartbeatsSent;
  6: i64 heartbeatsReceived;
  7: i64 parseErrors;
  8: i32 neighborsEstablished;
  9: i32 totalNeighbors;
  10: list<SparkNeighborState> neighbors;
}

// Counts from the DUT's currently computed route database, used to confirm the
// DUT converged after topology injection.
struct RouteCounts {
  1: i64 unicastRoutes;
  2: i64 mplsRoutes;
}

struct TestStatus {
  // True iff a session is active (between startTest and stopTest).
  1: bool running;
  // The config the active session was started with. Unset when !running.
  2: optional ScaleTestConfig activeConfig;
  // Names of nodes currently downed via downNode().
  3: list<string> downedNodes;
  // Links currently downed via downLink() (and not subsumed by a downed node).
  4: list<LinkRef> downedLinks;
  // True iff the injector's underlying thrift channel to the DUT is connected.
  5: bool dutConnected;
  // Seconds since the active session started. Unset when !running.
  6: optional i64 elapsedSec;
  // Number of simulated Spark neighbors the active session is driving.
  // Unset when !running or when simulateNeighbors=false.
  7: optional i32 neighborCount;
}

enum SetupErrorReason {
  TOPOLOGY_INVALID = 1,
  DUT_UNREACHABLE = 2,
  PORT_BIND_FAILED = 3,
  INJECTION_FAILED = 4,
  INTERNAL = 99,
}

exception AlreadyRunningError {
  @thrift.ExceptionMessage
  1: string message;
}
exception NotRunningError {
  @thrift.ExceptionMessage
  1: string message;
}
exception UnknownNodeError {
  @thrift.ExceptionMessage
  1: string message;
}
exception UnknownAdjacencyError {
  @thrift.ExceptionMessage
  1: string message;
}
exception SetupError {
  1: SetupErrorReason reason;
  @thrift.ExceptionMessage
  2: string message;
}

service ScaleTestServer {
  void startTest(1: ScaleTestConfig config) throws (
    1: AlreadyRunningError already,
    2: SetupError setup,
  );
  void stopTest() throws (1: NotRunningError notRunning);
  TestStatus getTestStatus();

  list<string> listNodes() throws (1: NotRunningError notRunning);

  void downNode(1: string nodeName) throws (
    1: NotRunningError notRunning,
    2: UnknownNodeError unknown,
  );
  void upNode(1: string nodeName) throws (
    1: NotRunningError notRunning,
    2: UnknownNodeError unknown,
  );
  void downLink(1: string localNode, 2: string remoteNode) throws (
    1: NotRunningError notRunning,
    2: UnknownNodeError unknown,
    3: UnknownAdjacencyError unknownAdj,
  );
  void upLink(1: string localNode, 2: string remoteNode) throws (
    1: NotRunningError notRunning,
    2: UnknownNodeError unknown,
    3: UnknownAdjacencyError unknownAdj,
  );

  // Bulk variants: down/up a SET of nodes (or links) as ONE coherent KvStore
  // update wave so the DUT sees a single convergence event (e.g. "fail a pod"),
  // not N sequential ones. Validation is atomic — if ANY member is invalid the
  // whole batch is rejected and nothing is applied. Each affected neighbor's adj
  // DB is rebuilt exactly once against the final downed state.
  void downNodes(1: list<string> nodeNames) throws (
    1: NotRunningError notRunning,
    2: UnknownNodeError unknown,
  );
  void upNodes(1: list<string> nodeNames) throws (
    1: NotRunningError notRunning,
    2: UnknownNodeError unknown,
  );
  void downLinks(1: list<LinkRef> links) throws (
    1: NotRunningError notRunning,
    2: UnknownNodeError unknown,
    3: UnknownAdjacencyError unknownAdj,
  );
  void upLinks(1: list<LinkRef> links) throws (
    1: NotRunningError notRunning,
    2: UnknownNodeError unknown,
    3: UnknownAdjacencyError unknownAdj,
  );

  // Flap a single link: downLink -> wait intervalMs -> upLink, repeated `cycles`
  // times. A link flap is bidirectional, so each cycle withdraws/restores BOTH
  // endpoints' adjacencies (reuses the symmetric downLink/upLink path).
  //
  // FIRE-AND-FORGET: the call validates the link synchronously (so bad args /
  // not-running surface immediately) then runs the flap on a background worker
  // and returns right away — the operator can keep issuing commands and watch
  // getTestStatus(), whose downedLinks reflects the in-flight flap. cycles <= 0
  // is a no-op; a negative intervalMs is treated as 0. The link ends UP after a
  // clean flap; if the session is stopped mid-flap it may be left down.
  void flapLink(
    1: string localNode,
    2: string remoteNode,
    3: i32 cycles,
    4: i32 intervalMs,
  ) throws (
    1: NotRunningError notRunning,
    2: UnknownNodeError unknown,
    3: UnknownAdjacencyError unknownAdj,
  );

  // Bulk fire-and-forget flap: flap a SET of links together — each cycle drives
  // the symmetric bulk downLinks/upLinks so all listed links go down (and back
  // up) as one coherent wave. Same fire-and-forget / validation semantics as
  // flapLink.
  void flapLinks(
    1: list<LinkRef> links,
    2: i32 cycles,
    3: i32 intervalMs,
  ) throws (
    1: NotRunningError notRunning,
    2: UnknownNodeError unknown,
    3: UnknownAdjacencyError unknownAdj,
  );

  // Empty regex = use the daemon's default counter set.
  map<string, i64> getDutCounters(1: string regexFilter);

  // Structured report of the simulated Spark neighbors: aggregate packet stats
  // plus the per-neighbor table. All-zero / empty when simulateNeighbors is
  // false for the active session.
  NeighborStats getNeighborStats() throws (1: NotRunningError notRunning);

  // Query the DUT's currently computed route database and return route counts.
  // Pull-based equivalent of the legacy post-injection route verification; the
  // operator calls it once the DUT is expected to have converged. Returns zero
  // counts if the injector's channel to the DUT is not connected.
  RouteCounts verifyRoutes() throws (1: NotRunningError notRunning);
}
