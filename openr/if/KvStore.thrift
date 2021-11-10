/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.KvStore
namespace py openr.KvStore
namespace py3 openr.thrift
namespace lua openr.KvStore
namespace wiki Open_Routing.Thrift_APIs.KvStore

struct KvStoreFloodRate {
  1: i32 flood_msg_per_sec;
  2: i32 flood_msg_burst_size;
}

/**
 * KvStoreConfig is the centralized place to configure
 */
struct KvStoreConfig {
  /**
   * Set the TTL (in ms) of a key in the KvStore. For larger networks where
   * burst of updates can be high having high value makes sense. For smaller
   * networks where burst of updates are low, having low value makes more sense.
   */
  1: i32 key_ttl_ms = 300000;

  /**
   * Set node_name attribute to uniquely differentiate KvStore instances.
   *
   * ATTN: the behavior of multiple nodes sharing SAME node_name is NOT defined.
   */
  2: string node_name;

  3: i32 ttl_decrement_ms = 1;

  4: optional KvStoreFloodRate flood_rate;

  /**
   * Sometimes a node maybe a leaf node and have only one path in to network.
   * This node does not require to keep track of the entire topology. In this
   * case, it may be useful to optimize memory by reducing the amount of
   * key/vals tracked by the node. Setting this flag enables key prefix filters
   * defined by key_prefix_filters. A node only tracks keys in kvstore that
   * matches one of the prefixes in key_prefix_filters.
   */
  5: optional bool set_leaf_node;

  /**
   * This comma separated string is used to set the key prefixes when key prefix
   * filter is enabled (See set_leaf_node). It is also set when requesting KEY_DUMP
   * from peer to request keys that match one of these prefixes.
   */
  6: optional list<string> key_prefix_filters;
  7: optional list<string> key_originator_id_filters;

  /**
   * Set this true to enable flooding-optimization, Open/R will start forming
   * spanning tree and flood updates on formed SPT instead of physical topology.
   * This will greatly reduce kvstore updates traffic, however, based on which
   * node is picked as flood-root, control-plane propagation might increase.
   * Before, propagation is determined by shortest path between two nodes. Now,
   * it will be the path between two nodes in the formed SPT, which is not
   * necessary to be the shortest path. (worst case: 2 x SPT-depth between two
   * leaf nodes). data-plane traffic stays the same.
   */
  8: optional bool enable_flood_optimization;

  /**
   * Set this true to let this node declare itself as a flood-root. You can set
   * multiple nodes as flood-roots in a network, in steady state, Open/R will
   * pick optimal (smallest node-name) one as the SPT for flooding. If optimal
   * root went away, Open/R will pick 2nd optimal one as SPT-root and so on so
   * forth. If all root nodes went away, Open/R will fall back to naive flooding.
   */
  9: optional bool is_flood_root;

  /**
   * Mark control plane traffic with specified IP-TOS value.
   * Valid range (0, 256) for making.
   * Set this to 0 if you don't want to mark packets.
   */
  10: optional i32 ip_tos;

  /**
  * TODO: remove this after ZMQ is removed from openr
  * Set buffering size for KvStore socket communication. Updates to neighbor node during
  * flooding can be buffered upto this number. For larger networks where burst of updates
  * can be high having high value makes sense. For smaller networks where burst of updates
  * are low, having low value makes more sense. Defaults to 65536.
  */
  101: i32 zmq_hwm = 65536;

  /**
   * TODO: remove this after dual msg is transmitted via thrift by default
   * Temp var to enable dual msg exchange over thrift channel.
   */
  200: bool enable_thrift_dual_msg = false;
} (cpp.minimize_padding)
