/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

# NOTE - Intentionally keeping namespace different from actual fb303
# in-order to avoid compile errors for use with internal projects

namespace cpp facebook.fb303
namespace py fb303

/**
 * Common status reporting mechanism across all services
 */
enum fb_status {
  DEAD = 0,
  STARTING = 1,
  ALIVE = 2,
  STOPPING = 3,
  STOPPED = 4,
  WARNING = 5,
}

service FacebookService {
  /**
   * Returns a descriptive name of the service
   */
  string getName()

  /**
   * Returns the unix time that the server has been running since
   */
  i64 aliveSince() (priority = 'IMPORTANT'),

  /**
   * Gets the status of this service
   */
  fb_status getStatus() (priority='IMPORTANT'),

  /**
   * Gets the counters for this service
   */
  map<string, i64> getCounters(),

  /**
   * Gets a subset of counters which match a
   * Perl Compatible Regular Expression for this service
   */
  map<string, i64> getRegexCounters(1: string regex)

  /**
   * Get counter values for a specific list of keys.  Returns a map from
   * key to counter value; if a requested counter doesn't exist, it won't
   * be in the returned map.
   */
  map<string, i64> getSelectedCounters(
    1: list<string> keys
  ) (priority = 'IMPORTANT')

  /**
   * Gets the value of a single counter
   */
  i64 getCounter(1: string key) (priority = 'IMPORTANT')

  /**
   * Gets the exported string values for this service
   */
  map<string, string> getExportedValues() (priority = 'IMPORTANT')

  /**
   * Gets a subset of exported values which match a
   * Perl Compatible Regular Expression for this service
   */
  map<string, string> getRegexExportedValues(1: string regex)

  /**
   * Get exported strings for a specific list of keys.  Returns a map from
   * key to string value; if a requested key doesn't exist, it won't
   * be in the returned map.
   */
  map<string, string> getSelectedExportedValues(
    1: list<string> keys
  ) (priority = 'IMPORTANT')

  /**
   * Gets the value of a single exported string
   */
  string getExportedValue(1: string key) (priority = 'IMPORTANT')
}
