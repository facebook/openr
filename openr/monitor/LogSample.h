/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <set>
#include <string>
#include <vector>

#include <folly/Range.h>
#include <folly/dynamic.h>

namespace openr {

/**
 * This class represents a loosely structured data for logging. Facilitates
 * application to easily create the events with many attriutes and send it
 * over wire to some central monitoring service.
 *
 * This class is strictly meant to make things easier for services to create
 * samples and serialize them to json objects.
 *
 * Example usecase:
 *    LogSample sample(std::chrono::system_clock::now());
 *    sample.addString("event", "NEIGHBOR_UP");
 *    sample.addString("neighbor", "node111");
 *    sample.addString("interface", "po1201");
 *    sample.addInt("rtt_ms", 3000);
 *    sample.addStringVector("addresses", {"1.2.3.4", "fe80::1"});
 *
 *    auto json = sample.toJson();
 *    myMonitoringServiceClient.send(sample.json)
 *
 * NOTE: Timestamp is critical part of Sample as it tells when event/log was
 * generated. It must be a measurement related to system clock (no steady
 * clock) to get absolute notion of time.
 */
class LogSample {
 public:
  /**
   * Construct a new LogSample with current system timestamp.
   */
  LogSample();

  /**
   * Construct a new LogSample with a custom system timestamp.
   */
  explicit LogSample(std::chrono::system_clock::time_point timestamp);

  LogSample(
      folly::dynamic json, std::chrono::system_clock::time_point timestamp);

  static LogSample fromJson(const std::string& json);

  /**
   * Get json representation of the Sample. Can easily be sent to monitoring
   * service over write.
   */
  std::string toJson() const;

  /**
   * Get the timestamp associated with this sample.
   */
  std::chrono::system_clock::time_point
  getTimestamp() const {
    return timestamp_;
  }

  /**
   * APIs to add different types of values
   */
  void addInt(folly::StringPiece key, int64_t value);
  void addDouble(folly::StringPiece key, double value);
  void addString(folly::StringPiece key, folly::StringPiece value);
  void addStringVector(
      folly::StringPiece key, const std::vector<std::string>& values);
  void addStringTagset(
      folly::StringPiece key, const std::set<std::string>& tags);

  /**
   * APIs to get different types of values. Throws exception if value doesn't
   * exists.
   */
  int64_t getInt(folly::StringPiece key) const;
  double getDouble(folly::StringPiece key) const;
  std::string getString(folly::StringPiece key) const;
  std::vector<std::string> getStringVector(folly::StringPiece key) const;
  std::set<std::string> getStringTagset(folly::StringPiece key) const;

  /**
   * APIs to test existance of various values.
   */
  bool isIntSet(folly::StringPiece key) const;
  bool isDoubleSet(folly::StringPiece key) const;
  bool isStringSet(folly::StringPiece key) const;
  bool isStringVectorSet(folly::StringPiece key) const;
  bool isStringTagsetSet(folly::StringPiece key) const;

 private:
  bool isInnerValueSet(
      folly::StringPiece keyType, folly::StringPiece key) const;
  const folly::dynamic& getInnerValue(
      folly::StringPiece keyType, folly::StringPiece key) const;

  // Internal json representation of this sample
  folly::dynamic json_ = folly::dynamic::object;

  // Timepoint associated with this sample
  std::chrono::system_clock::time_point timestamp_;
};

} // namespace openr
