// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <chrono>

#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>

namespace openr {
namespace fbmeshd {

class Notifier {
 public:
  Notifier(folly::EventBase* evb, std::chrono::milliseconds interval);

  Notifier() = delete;
  ~Notifier() = default;
  Notifier(const Notifier&) = delete;
  Notifier(Notifier&&) = delete;
  Notifier& operator=(const Notifier&) = delete;
  Notifier& operator=(Notifier&&) = delete;

 private:
  void doNotify();
  std::unique_ptr<folly::AsyncTimeout> notifierTimer_;
};
} // namespace fbmeshd
} // namespace openr
