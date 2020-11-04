# Versioning

---

This page describes the major changes inside Open/R including:

- New feature support being added
- Feature support being dropped
- Non backward-compatible changes in message format/CLI interface/etc.

## Overview of Version Usage

---

Open/R use `SparkHelloMsg` to carry `version` attribute during neighbor
discovery process. Upon receiving `SparkHelloMsg`, `Spark` instance from peer
will compare the version received against `lowestSupportedVersion` configured
locally. Both `version` and `lowestSupportedVersion` are of `uint32_t` type.

In Open/R, version will be specified according to the timestamp with granularity
of `day`. Samples are:

- version: `20200825`
- lowestSupportedVersion: `20200214`

See [Spark.md](../Protocol_Guide/Spark.md)
for more details about neighbor discivery mechanism inside `Spark`.

### Backward Compatibility

---

There are multiple aspects of backward comaptibility concern within Open/R:

- Message exchanged between `Spark` instances for adjacency establishment;
- Message exchanged between `KvStore` instances for data consistency globally;
- `breeze` CLI interface between client and server;
- etc.

Open/R will maintain at least `6 months` of backward compatible version with
regarding to messgaes between `Spark` neighbor and `KvStore` peers. This is to
make sure core functionality of Open/R can be fulfilled without interruption.

### Version History

---

- Version 20200825

  - Platform publisher service with ZMQ PUB/SUB deprecated

- Version 20200801

  - System service deprecated

- Version 20200701

  - Area feature becomes mandatory

- Version 20200604

  - Old Spark feature deprecated

- Version 20200421

  - Spark AREA feature support

- Version 20191010

  - Spark2 feature support

- Version 20190805
  - Per prefix key feature support
