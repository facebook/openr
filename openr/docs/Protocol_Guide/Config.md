# Config

## Introduction

---

As the name suggests, this module provides library interface for Configuration,
passed by `--config`, that Open/R needs. It performs sanitization checks and
provide C++ wrapper over config object.

## APIs

---

For more information about config format, check out

- [if/OpenrConfig.thrift](https://github.com/facebook/openr/blob/master/openr/if/OpenrConfig.thrift)
- [if/BgpConfig.thrift](https://github.com/facebook/openr/blob/master/openr/if/BgpConfig.thrift)

### Accessor Methods

- Submodule config. e.g. `getSparkConfig()`, `getKvStoreConfig()`
- Optional field access - return false if not set. e.g. `isV4Enabled()`,
  `isSegmentRoutingEnabled()`
