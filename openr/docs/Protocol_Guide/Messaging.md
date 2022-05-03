# Messaging - The Glue that binds Modules

[Open/R Architecture](Architecture.md) explains the modular design of the
protocol and its benefit. `Messaging` enables these modules to interact with
each other. Interactions across module consist of various message types. This
library provides various `Queue` interfaces using which modules can write or
read messages.

## Design Principles

---

Our choice of design principles aims to reduce code complexity and support the
evolution of code with new asynchronous primitives. Queue types described in the
later section follows the below principles

- Message must be `strongly typed`. This ensures type safety and avoids
  serialize/de-serialize overhead for message exchange between modules
- Queue is `monomorphic`. Aka each queue only supports send/receive of a unique
  message type. For sending multiple message types `std::variant` can be used.
- Messages are `uni-directional` and there is no request-reply pattern. This
  forces developers to design and test around this constraint. This design
  choice `avoids` the possibility of `dead-locks` between threads.
- `Writes are non-blocking`. If no readers are available then a message will be
  queued at the cost of memory. Crazy writers with slow readers could clog the
  memory but we don't expect such behavior. However, on the upside, this ensures
  that the module will never be blocked on write.
- `Reads` could be `blocking` or `asynchronous` as per the application's choice.
  It supports asynchronous reads on `folly::fiber` and `std::coroutine`.

> NOTE: We're hoping to move towards `std::coroutine` for all asynchronous
> communication in near future.

## Queue Architecture

---

There are two types of Queue that we support, namely `RWQueue<T>` and
`ReplicateQueue<T>`. Both queues provides `RQueue<T>`, the read-only interface
for the readers. It ensures that the reader can never write into the queue.

### RWQueue

Queue supports multiple readers and writers. `std::mutex` is used to protect
data from thread concurrency. Code in the critical path is minimal and ensures
that readers/writers will never block each other.

Some notable points about RWQueue

- Supports multiple readers and writers
- Enqueued message can only be read once i.e. messages will be distributed
  across readers
- Provides fairness across the multiple readers

### ReplicateQueue

As the name suggests, it supports one to many messaging patterns. It is built on
the top of `RWQueue`. It holds one such instance of it for every reader
underneath. Readers must be explicitly created by invoking `.getReader()` API.

Some notable points about ReplicateQueue

- By definition of replication message must be `copy constructible`
- Supports `multiple` readers and writers
- Every reader gets `every` written message into the queue
- All `readers` must be `created` before the first element is written to ensure
  `lossless` message replication
- `Writer` pays the `cost of replication`. Use of `shared_ptr<>` would greatly
  reduce the replication cost when the message is large and there are many
  readers

### Performance

This is planned work and we'll share some initial benchmark results for the
queue indicative of its performance. As of now, it hasn't been the bottleneck in
the context of performance.

## Learn More

---

Header files are well documented. Use that as a guide for using the messaging
library in your code. You can explore `openr/messaging/tests/` for examples.
