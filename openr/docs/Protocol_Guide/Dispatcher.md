# Dispatcher

## Introduction
---

`Dispatcher` is the module that is a proxy between `KvStore` and the other
modules. Modules that subscribe to `Dispatcher` pass filters to allow
`Dispatcher` to send deltas from `KvStore` that are relevant to that module.

## General Workflow
---

Below the diagram describes the flow of the delta changes coming from `KvStore`
to the subscribing modules.
![Slide2](https://user-images.githubusercontent.com/54754741/184961218-4f66c7c4-eab3-4737-a168-985508824c96.jpg)

## Operations
---

Below we describe the pseudocode of the `Dispatcher` module

1. `Read` incoming deltas from `KvStore` via a `ReplicateQueue`
2. `Copy` the incoming publication and only add keys that pass through one of a
   subscriber's list of filters
3. `Replicate` non-empty publications to modules that are interested in
   receiving the publication
