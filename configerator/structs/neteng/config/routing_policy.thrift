/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 neteng.config.routing_policy
namespace py neteng.config.routing_policy
namespace go neteng.config.routing_policy
namespace py3 neteng.config

enum Operator {
  add = 1,
  rewrite = 2,
  remove = 3,
  exact = 4,
  subset = 5,
  prepend = 6,
}

enum Protocol {
  bgp = 1,
  openr = 2,
}

enum RuleAction {
  allow = 1,
  deny = 2,
  nextRule = 3,
  gotoRule = 4, // Future possibility
}

enum RuleCondition {
  matchAll = 1,
  matchAny = 2,
  // Future possiblities
  matchNone = 3,
}

/**
 *  START OF SUB-SECTION "policyConfig"
 */

//
// BGP structs
//

enum BgpOriginValues {
  IGP = 0,
  EGP = 1,
  INCOMPLETE = 2,
}

struct BgpCommunity {
  1: string _description;
  2: optional string _legacyAlias;
  3: list<string> communitySet;
}

struct BgpCommunities {
  1: string _description;
  2: string _protocol;
  3: Protocol _protocol_id;
  4: optional string _revision;
  5: map<string/* community name */ , BgpCommunity> objects;
}

struct BgpLocalPref {
  1: string _description;
  2: optional string _legacyAlias;
  3: i16 localPref;
}

struct BgpLocalPrefs {
  1: string _description;
  2: string _protocol;
  3: Protocol _protocol_id;
  4: optional string _revision;
  5: map<string/* localpref name */ , BgpLocalPref> objects;
}

struct BgpOrigin {
  1: string _description;
  2: optional string _legacyAlias;
  3: BgpOriginValues origin;
}

struct BgpOrigins {
  1: string _description;
  2: string _protocol;
  3: Protocol _protocol_id;
  4: optional string _revision;
  5: map<string/* origin name */ , BgpOrigin> objects;
}

struct BgpPath {
  1: string _description;
  2: optional string _legacyAlias;
  3: list<i32> asnSequence;
}

struct BgpPaths {
  1: string _description;
  2: string _protocol;
  3: Protocol _protocol_id;
  4: optional string _revision;
  5: map<string/* path name */ , BgpPath> objects;
}

//
// Openr structs
//

enum OpenrPrefixForwardingTypeValues {
  IP = 0,
  SR_MPLS = 1,
}

enum OpenrPrefixForwardingAlgorithmValues {
  SP_ECMP = 0,
  KSP2_ED_ECMP = 1,
}

struct OpenrTag {
  1: string _description;
  2: list<string> tagSet;
}

struct OpenrTags {
  1: string _description;
  2: optional string _revision;
  3: map<string/* tag set name */ , OpenrTag> objects;
}

struct OpenrAreaStack {
  1: string _description;
  2: list<string> areaStack;
}

struct OpenrAreaStacks {
  1: string _description;
  2: optional string _revision;
  3: map<string/* area stack name */ , OpenrAreaStack> objects;
}

struct OpenrPathPreference {
  1: string _description;
  3: i32 pathPreference;
}

struct OpenrPathPreferences {
  1: string _description;
  2: optional string _revision;
  3: map<string/* path pref name */ , OpenrPathPreference> objects;
}

struct OpenrSourcePreference {
  1: string _description;
  3: i32 sourcePreference;
}

struct OpenrSourcePreferences {
  1: string _description;
  2: optional string _revision;
  3: map<string/* admin pref name */ , OpenrSourcePreference> objects;
}

struct OpenrPrefixForwardingType {
  1: string _description;
  2: OpenrPrefixForwardingTypeValues type;
}

struct OpenrPrefixForwardingTypes {
  1: string _description;
  4: optional string _revision;
  5: map<string/* type name */ , OpenrPrefixForwardingType> objects;
}

struct OpenrPrefixForwardingAlgorithm {
  1: string _description;
  2: OpenrPrefixForwardingAlgorithmValues algorithm;
}

struct OpenrPrefixForwardingAlgorithms {
  1: string _description;
  4: optional string _revision;
  5: map<string/* algorithm name */ , OpenrPrefixForwardingAlgorithm> objects;
}

//
// Protocol generic structs
//

struct NextHop {
  1: string _description;
  2: optional string _legacyAlias;
  3: optional string addressIPv4;
  4: optional string addressIPv6;
}

struct NextHops {
  1: string _description;
  2: optional string _revision;
  3: map<string/* nexthop name */ , NextHop> objects;
}

struct UserVariable {
  1: string _description;
  2: optional string _legacyAlias;
  3: string value;
}

struct UserVariables {
  1: string _description;
  2: optional string _revision;
  3: map<string/* variable name */ , UserVariable> objects;
}

struct PolicyDefinitions {
  1: optional BgpCommunities bgpCommunity;
  2: optional BgpLocalPrefs bgpLocalPref;
  3: optional BgpOrigins bgpOrigin;
  4: optional BgpPaths bgpPath;

  5: optional NextHops nextHop;
  6: optional UserVariables userVariable;

  7: optional OpenrTags openrTag;
  8: optional OpenrAreaStacks openrAreaStack;
  9: optional OpenrPathPreferences openrPathPreference;
  10: optional OpenrSourcePreferences openrSourcePreference;
  11: optional OpenrPrefixForwardingAlgorithms openrPrefixForwardingAlgorithm;
  12: optional OpenrPrefixForwardingTypes openrPrefixForwardingType;
}

// I wish this could be union of
// PolicyCriteria/PrefixFilterCriteria/RegexFilterCriteria
struct FilterCriteria {
  1: optional string _remark;
  2: optional bool alwaysMatch;

  //
  // PolicyCriteria: match on different attribute field, use in
  //                  routeOriginationPolicy, routePropagationPolicy
  //

  // protocol generic (index range 3: )
  3: optional list<string> prefixFilters;
  4: optional string nextHop;

  // bgp (index range 20:)
  20: optional list<string> bgpCommunities;
  21: optional list<string> bgpCommunityFilters;
  22: optional string bgpLocalPref;
  23: optional string bgpOrigin;
  24: optional list<string> bgpPathFilters;
  25: optional string bgpPath;
  26: optional list<string> bgpPrefixFilters (deprecated);

  // openr (index range 40:)
  40: optional list<string> openrTags;
  41: optional list<string> openrTagFilters;
  /* NOTE: use ' ' as delimiter in area stack regex.
   * We recommend to use openrTagFilters as much as possible.
   * areaStackFilter can be expensive as it flattens attributes
   * area_stack for regex match every time.
   */
  42: optional list<string> openrAreaStackFilters;
  43: optional string openrAreaStack;
  44: optional string openrPathPreference;
  45: optional string openrSourcePreference;
  46: optional string openrPrefixForwardingType; // IP|MPLS
  47: optional string openrPrefixForwardingAlgorithm; // ECMP|KSP2

  90: optional string condition;
  91: optional RuleCondition condition_id;
  // only exact is allowed here as an operation, for exact match on community
  92: optional string operation;
  93: optional Operator operation_id;

  //
  // PrefixFilterCriteria: fields only applicable to define prefixFilter
  //
  100: optional string basePrefixIPv4;
  101: optional string basePrefixIPv6;

  102: optional i16 maxLengthIPv4;
  103: optional i16 maxLengthIPv6;
  104: optional i16 minLengthIPv4;
  105: optional i16 minLengthIPv6;

  //
  // RegexFilterCriteria: applicable only in bgpPathFilters, communityFilters
  //
  106: optional string regex;
}

struct FilterTransform {
  1: optional string _remark;

  // bgp (index range 2:)
  2: optional list<string> bgpCommunities;
  3: optional list<string> bgpCommunityFilters;
  4: optional string bgpLocalPref;
  5: optional string bgpOrigin;
  6: optional string nextHop;
  7: optional string bgpPath;

  // openr (index range 20:)

  /*
   * valid operators: rewrite, add, remove
   */
  20: optional list<string> openrTags;
  /*
   * match on tag regex in openrTagFilters, remove matched portion
   * valid operators: remove
   */
  21: optional list<string> openrTagFilters;
  /*
   * valid operators: rewrite, add
   */
  22: optional i64 openrDistance;
  /*
   * valid operators: rewrite
   */
  23: optional string openrPathPreference;
  /*
   * valid operators: rewrite
   */
  24: optional string openrPrefixForwardingType; // IP|MPLS
  /*
   * valid operators: rewrite
   */
  25: optional string openrPrefixForwardingAlgorithm; // ECMP|KSP2
  /*
   * minimum nexthops required to program a route in fib
   * valid operators: rewrite
   */
  26: optional i64 openrMinNexthop;
  /*
   * flag that indicates if prependLabel attribute is accepted. If not, the
   * attribute is reset.
   * Note: openrAcceptPrependLabel does not need operators.
   */
  27: optional bool openrAcceptPrependLabel;

  // not all operation is applicable to all fields:
  // e.g. subset/add are only applicable to bgpLocalPref
  90: optional string operation;
  91: optional Operator operation_id;
}

struct FilterRule {
  1: string _description;
  2: string _name;
  3: optional string _remark;

  4: string condition;
  5: RuleCondition condition_id;
  6: list<FilterCriteria> criteria;

  7: string ruleMatchAction;
  8: RuleAction ruleMatchAction_id;
  9: string ruleMissAction;
  10: RuleAction ruleMissAction_id;
  11: optional string targetRule;
  /* placeholder for future gotoRule action */

  //
  // transforms are being used to change attributes upon a match:
  //    only applicable im routePropagationPolicy, routeOriginationPolicy
  //
  12: optional list<FilterTransform> transform;
}

struct Filter {
  1: string _description;
  2: optional string _legacyAlias;
  3: optional string _protocol;
  4: optional Protocol _protocol_id;
  5: optional string _remark;

  /* currently applicable only in prefixFilter */
  6: optional bool ignoreIPv4;
  7: optional bool ignoreIPv6;

  8: list<FilterRule> ruleset;
  /* empty ruleset triggers rulesetMissAction
                                  (no rules matched) */
  11: string rulesetMissAction;
  12: RuleAction rulesetMissAction_id;
}

struct Filters {
  1: string _description;
  2: optional string _protocol;
  3: optional Protocol _protocol_id;
  4: optional string _revision;
  5: map<string/* filter name */ , Filter> objects;
}

struct PolicyFilters {
  // bgp filters
  1: optional Filters bgpCommunityFilter;
  2: optional Filters bgpPathFilter;
  // protocol generic filters
  3: optional Filters prefixFilter;
  // policies
  4: optional Filters routeOriginationPolicy;
  5: optional Filters routePropagationPolicy;
  // openr filters
  6: optional Filters openrAreaStackFilters;
  7: optional Filters openrTagFilters;
}

struct PolicyConfig {
  1: string _description;
  2: PolicyDefinitions definitions;
  3: PolicyFilters filters;
}
