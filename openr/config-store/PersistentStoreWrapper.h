/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/config-store/PersistentStore.h>

namespace openr {

class PersistentStoreWrapper {
 public:
  explicit PersistentStoreWrapper(const unsigned long tid);

  // Destructor will try to save DB to disk before destroying the object
  ~PersistentStoreWrapper() {
    stop();
  }

  PersistentStore*
  operator->() {
    return store_.get();
  }

  /**
   * Synchronous APIs to run and stop PersistentStore. This creates a thread
   * and stop it on destruction.
   *
   * Synchronous => function call with return only after thread is
   *                running/stopped completely.
   */
  void run() noexcept;
  void stop();

 public:
  const std::string filePath;

 private:
  std::unique_ptr<PersistentStore> store_;

  // Thread in which PersistentStore will be running.
  std::thread storeThread_;
};
} // namespace openr
