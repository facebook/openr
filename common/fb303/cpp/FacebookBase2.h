/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <time.h>

#include <common/fb303/if/gen-cpp2/FacebookService.h>

namespace facebook {
namespace fb303 {

class FacebookBase2 : virtual public cpp2::FacebookServiceSvIf {
 public:
  explicit FacebookBase2(const char* name)
      : startTime_(time(nullptr)), name_(name) {}

  void
  getName(std::string& _return) override {
    _return = name_;
  }

  int64_t
  aliveSince() override {
    return startTime_;
  }

  void getCounters(std::map<std::string, int64_t>& _return) override;

  void getExportedValues(std::map<std::string, std::string>& _return) override;
  void getRegexExportedValues(
      std::map<std::string, std::string>& _return,
      std::unique_ptr<std::string> regex) override;
  void getSelectedExportedValues(
      std::map<std::string, std::string>& _return,
      std::unique_ptr<std::vector<std::string>> keys) override;
  void getExportedValue(
      std::string& _return, std::unique_ptr<std::string> key) override;

 private:
  const time_t startTime_;
  const std::string name_;
};

} // namespace fb303
} // namespace facebook
