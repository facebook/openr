/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.BgpConfig
namespace py openr.BgpConfig
namespace py3 openr.thrift
namespace wiki Open_Routing.Thrift_APIs.BgpConfig

struct BgpPeerTimers {
  1: i32 hold_time_seconds,
  2: i32 keep_alive_seconds,
  3: i32 out_delay_seconds,
  4: i32 withdraw_unprog_delay_seconds,
  5: optional i32 graceful_restart_seconds,
  6: optional i32 graceful_restart_end_of_rib_seconds,
}

struct RouteLimit {
    /* Maximum routes allow for this peer */
    1: i64 max_routes = 12000, // default 12000
    /* Only emit warning when max route limit exceeded*/
    2: bool warning_only = false, // default false
    /* # of routes at which warning is to be issued. */
    3: i64 warning_limit = 0, // default 0, not using warning_limit
} (cpp.minimize_padding)

/**
 * AdvertiseLinkBandwidth:
 * - NONE: Do not advertise Link Bandwidth Ext Community to this peer/group
 * - AGGREGATE: Advertise aggregate of received Link Bandwidth values of paths
 *   we use in ECMP (only if received from all speakers)
 */
enum AdvertiseLinkBandwidth {
  NONE = 0,
  AGGREGATE = 1,
}

/**
 * AddPathCapability
 * None: do not use add path feature
 * RECEIVE: only receive additional path from peer.
 * SEND: only send additional path from peer.
 * BOTH: send/receive additional paths to/from peer.
 */
enum AddPath {
  NONE = 0,
  RECEIVE = 1,
  SEND = 2,
  BOTH = 3,
}

struct PeerGroup {
  1: string name
  2: optional string description
  /* Basic peer config */
  3: optional i32 remote_as
  4: optional string local_addr
  5: optional string next_hop4
  6: optional string next_hop6
  7: optional bool enabled;
  8: optional i32 router_port_id;

  /* BgpPgFeatures in generic_switch_config.thrift */
  10: optional bool is_passive
  11: optional bool is_confed_peer
  12: optional bool is_rr_client
  13: optional bool next_hop_self
  14: optional bool remove_private_as;

  /* BgpPgMultiprotocolAttrs in generic_switch_config.thrift */
  20: optional bool disable_ipv4_afi
  21: optional bool disable_ipv6_afi
  /* Policy name for ingress and egress policies */
  // 22: optional string ingress_policy_name;
  // 23: optional string egress_policy_name;
  /* BgpPgTimers in generic_switch_config.thrift */
  24: optional BgpPeerTimers bgp_peer_timers
  /* This is peer_tag/type in BgpPeer */
  25: optional string peer_tag
  /* Local-AS# RFC-7705 */
  26: optional i32 local_as
  27: optional AdvertiseLinkBandwidth advertise_link_bandwidth;
  /* Route caps for pre & post filter prefixes */
  28: optional RouteLimit pre_filter;
  29: optional RouteLimit post_filter;
  /* Enable stateful HA to preserve preIn during restart */
  30: optional bool enable_stateful_ha;

  31: optional AddPath add_path;
} (cpp.minimize_padding)

/**
 * The configuration of a single BGP peer
 */
struct BgpPeer {
  /**
   * the peer's BGP ASN
   */
  1: i32 remote_as
  /**
   * local address to connect/listen on
   */
  2: string local_addr
  /**
   * remote address to connect to or accept connections from
   * this could be either address or prefix spec (e.g. x.x.x.x/24)
   * the latter only works for passive listening sessions
   */
  3: string peer_addr
  /**
   * next-hop values to use in outgoing updates
   */
  4: string next_hop4
  5: string next_hop6
  /**
   * description for the peer, e.g. csw05a.frc1
   */
  6: optional string description
  /**
   * passive peer will listen for incoming connections
   * the port to bind to is the listen_port from global config
   * default is to actively connect to the peer
   */
  7: optional bool is_passive
  /**
   * remote confederation as number
   */
  8: optional bool is_confed_peer

  /**
   * peer type
   */
  9: optional string type
 10: optional string peer_id
 /**
  * whether we should reflect this peer's routes
  */
 11: optional bool is_rr_client;
 /**
  * the tag to group similar peers
  */
 12: optional string peer_tag
 /**
  * whether we should set next-hop to self to this peer
  * this works for all AFI's.
  */
 13: optional bool next_hop_self
 /**
  * By default all AFI's are enabled, below allows to disable
  * them selectively. This is done for backward compat with
  * old configs.
  */
  14: optional bool disable_ipv4_afi
  15: optional bool disable_ipv6_afi

  /**
   * the port that this peer is found on. This is used to
   * remove peers quickly in case the port goes down.
   */
  18: optional i32 router_port_id;
  19: optional BgpPeerTimers bgp_peer_timers

  /**
   * Whether we expect the peer to be present. (Not taken by BGP++)
   */
  20: optional bool enabled;

  /**
   * Remove private AS
   */
  21: optional bool remove_private_as;

  /**
   * Policy name for ingress and egress policies.
   */
  // 22: optional string ingress_policy_name;
  // 23: optional string egress_policy_name;

  /*
   * Local-AS number for this peer. Only valid for eBGP peers.
   * Overrides the global config's local-as for this peer.
   * Reference: RFC-7705
   */
  25: optional i32 local_as;

  26: optional AdvertiseLinkBandwidth advertise_link_bandwidth;

  /* Route caps for pre & post filter prefixes */
  27: optional RouteLimit pre_filter;
  28: optional RouteLimit post_filter;

  /* Enable stateful HA to preserve preIn during restart */
  29: optional bool enable_stateful_ha;

  /**
   * Peer Group name. peer config overwrites peer group config
   */
  99: optional string peer_group_name;

  /**
   * Flag to control whether we want to enable add_path
   * functionality to this peer.
   */
  100: optional AddPath add_path;
} (cpp.minimize_padding)

/**
 * BGP configuration
 */
struct BgpConfig {
  /**
   * this is normally the loopback IP
   */
  1: string router_id
  /**
   * local ASN is same for all peers for simplicity
   */
  2: i32 local_as
  /**
   * the list of BGP peers for the switch
   */
  5: list<BgpPeer> peers
  /**
   * the hold time for all BGP sessions
   */
  6: i32 hold_time = 30
  /**
   * the TCP port to listen on for passive sessions
   */
  7: i32 listen_port = 179
  /**
   * the confederation asn for this router, if any
   */
  8: optional i32 local_confed_as

  /**
   * Address to listen on, defaults to ::
   */
  10: string listen_addr = "::"

  13: optional i32 cold_start_convergence_seconds,
  14: optional i32 graceful_restart_convergence_seconds,

  /**
   * Policies
   */
  // 15: optional BgpPolicies policies

  16: optional list<PeerGroup> peer_groups

  /**
   * Use received link bandwidth extended community to compute UCMP paths
   */
  17: optional bool compute_ucmp_from_link_bandwidth_community

  18: i32 eor_time_s = 45
} (cpp.minimize_padding)
