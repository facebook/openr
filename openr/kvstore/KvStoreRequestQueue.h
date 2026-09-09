/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <string>
#include <type_traits>

#include <openr/common/Types.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {

/*
 * Keep exact FIFO behavior for normal production bursts. For example, 64
 * pending requests remain intact and the 65th activates suppression.
 */
inline constexpr size_t kKvStoreRequestSuppressionActivationThreshold{64};

/**
 * Classifies locally originated KvStore requests for reader-side suppression.
 *
 * Persist and Clear describe the desired eventual state of one raw
 * `(area, key)` and may replace an older pending request. Set is an operation:
 * every invocation may intentionally publish or advance a version, so it is a
 * per-key barrier. A Clear superseded by a pending Persist is intentionally
 * omitted: because neither request has been processed, peers need only observe
 * the final persisted value. The area length prefix makes the string encoding
 * unambiguous.
 */
inline messaging::StateSuppressionKey
getKvStoreRequestStateSuppressionKey(const KeyValueRequest& request) {
  return std::visit(
      [](const auto& requestValue) {
        const auto& area = requestValue.getArea().t;
        std::string key = std::to_string(area.size());
        key.push_back(':');
        key.append(area);
        key.append(requestValue.getKey());

        using Request = std::decay_t<decltype(requestValue)>;
        /*
         * Only requests representing desired persistent state may replace an
         * earlier pending request. Every other request type is a barrier so a
         * future variant cannot silently lose operation semantics.
         */
        constexpr bool suppressible =
            std::is_same_v<Request, PersistKeyValueRequest> ||
            std::is_same_v<Request, ClearKeyValueRequest>;
        constexpr auto action = suppressible
            ? messaging::StateSuppressionAction::REPLACE_PENDING
            : messaging::StateSuppressionAction::KEY_BARRIER;
        return messaging::StateSuppressionKey{std::move(key), action};
      },
      request);
}

/*
 * Creates the KvStore request reader with keyed state suppression only when
 * queue coalescing is enabled and the pending depth crosses the activation
 * threshold. The disabled path intentionally uses the original FIFO reader so
 * every request remains observable.
 */
inline messaging::RQueue<KeyValueRequest>
getKvStoreRequestQueueReader(
    messaging::ReplicateQueue<KeyValueRequest>& queue,
    bool enableQueueCoalescing,
    size_t activationThreshold =
        kKvStoreRequestSuppressionActivationThreshold) {
  if (!enableQueueCoalescing) {
    return queue.getReader("kvStore");
  }
  return queue.getReader(
      "kvStore",
      messaging::StateSuppressionPolicy<KeyValueRequest>{
          getKvStoreRequestStateSuppressionKey, activationThreshold});
}

} // namespace openr
