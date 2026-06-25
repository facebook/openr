# Troubleshooting OpenR

This guide covers the most common issues encountered when deploying and operating OpenR in open-source environments, along with diagnostics steps and known workarounds.

---

## Table of Contents

1. [Spark Neighbor Discovery Failures](#1-spark-neighbor-discovery-failures)
2. [OpenR Crash / Watchdog Kill with Many Neighbors](#2-openr-crash--watchdog-kill-with-many-neighbors)
3. [KVStore Subscription Timeout (`TTransportException`)](#3-kvstore-subscription-timeout-ttransportexception)
4. [Build Failures (`folly::coro::Task` not found)](#4-build-failures-follycoro-not-found)
5. [jemalloc Warning: `mallctl: not using jemalloc`](#5-jemalloc-warning-mallctl-not-using-jemalloc)
6. [Python CLI: `ModuleNotFoundError: No module named 'openr.thrift'`](#6-python-cli-modulenotfounderror)
7. [breeze CLI Crashes in Area-Aware Mode](#7-breeze-cli-crashes-in-area-aware-mode)
8. [Collecting Diagnostic Information](#8-collecting-diagnostic-information)

---

## 1. Spark Neighbor Discovery Failures

### Symptom

```
E0327 03:06:50 Spark.cpp:1055] [SparkHeartbeatMsg] Failed sending pkt towards: ff02::1
over: <interface> due to error: Invalid argument
```

This error appears in `SparkHelloMsg` and `SparkHeartbeatMsg` sends. It typically surfaces when a node has **more than ~10 directly connected neighbors**.

### Cause

The error is an `EINVAL` from the kernel's `sendmsg()` syscall on the IPv6 multicast socket. Two common root causes:

- **`SO_SNDBUF` exhaustion**: The UDP multicast socket's send buffer fills up when many interfaces fire their hello/heartbeat timers simultaneously. The kernel returns `EINVAL` (not `EAGAIN`) for multicast sockets when the ancillary data or send queue exceeds limits.
- **Interface MTU mismatch**: If any interface in the hello-send loop has an MTU smaller than the Spark packet size (which grows with neighbor count due to included neighbor state), `sendmsg` will return `EINVAL`.

### Diagnostics

```bash
# Check the effective SO_SNDBUF on UDP sockets
cat /proc/net/udp6

# Check MTU of all OpenR-managed interfaces
ip link show | grep -E 'mtu|state'

# Check kernel UDP send buffer limits
sysctl net.core.wmem_max
sysctl net.core.wmem_default
```

### Workarounds

1. **Increase the kernel UDP send buffer size:**
   ```bash
   sysctl -w net.core.wmem_max=8388608
   sysctl -w net.core.wmem_default=425984
   ```

2. **Verify all OpenR interfaces have consistent MTU ≥ 1500 bytes.** Spark packets scale with neighbor count; smaller MTUs cause `EINVAL` on `sendmsg`.

3. **Reduce hello interval** (`spark_hello_time_s`) to spread out sends — though this is a mitigation, not a fix.

See also: [Issue #147](https://github.com/facebook/openr/issues/147)

---

## 2. OpenR Crash / Watchdog Kill with Many Neighbors

### Symptom

OpenR is killed by the watchdog when the number of neighbors reaches ~32 or when RSS memory grows beyond configured limits, even after increasing `memory_limit_mb`.

### Cause

In older OpenR releases, memory usage scaled super-linearly with neighbor count due to:
- KVStore holding full adjacency tables for all neighbors in memory
- Spark maintaining per-neighbor state machines without bound

### Diagnostics

```bash
# Monitor OpenR memory in real time
watch -n 1 'cat /proc/$(pgrep openr)/status | grep -E "VmRSS|VmPeak"

# Check KVStore key count
breeze kvstore keys | wc -l

# Check current adjacencies
breeze lm adjacencies
```

### Workarounds

1. Upgrade to a recent OpenR build — memory scaling improvements were committed post-`rc-20190419`.
2. Tune `memory_limit_mb` in `openr_config.thrift` to a value that reflects actual RSS.
3. Reduce `kvstore_sync_interval_s` to avoid large batched KVStore updates that spike memory.

See also: [Issue #137](https://github.com/facebook/openr/issues/137)

---

## 3. KVStore Subscription Timeout (`TTransportException`)

### Symptom

```
terminate called after throwing an instance of
'apache::thrift::transport::TTransportException'
  what(): TTransportException: Timed out
```

This occurs when using `openr_kvstore_snooper` or any client calling `semifuture_subscribeAndGetAreaKvStores()`.

### Cause

The streaming subscription is established but the server-side stream ends immediately (stream-0 attached, then closed). This is typically caused by:
- A version mismatch between the client Thrift stubs and the running OpenR binary
- OpenR running in an area-aware mode while the client uses single-area subscription APIs

### Diagnostics

```bash
# Verify OpenR version vs client library version
openr --version
python3 -c "import openr; print(openr.__version__)"

# Test basic Thrift connectivity
breeze openr version
```

### Fix

Rebuild the Python client from the same source tree as your OpenR binary to ensure Thrift ABI compatibility.

See also: [Issue #136](https://github.com/facebook/openr/issues/136)

---

## 4. Build Failures: `folly::coro` Not Found

### Symptom

```
error: 'Task' in namespace 'folly::coro' does not name a template type
  folly::coro::Task co_validateNodeKey(
```

### Cause

Your system's `folly` installation does not include coroutine support. `folly::coro` requires:
- A C++17 compiler with coroutine TS support (GCC 10+, Clang 8+)
- Folly built with `FOLLY_HAS_COROUTINES=1`
- `libboost-context` installed

### Fix

```bash
# Rebuild folly from the getdeps build system to ensure coroutine support
cd openr/build
python3 fbcode_builder/getdeps.py build --only-repo folly

# Verify folly coroutine support
grep -r 'FOLLY_HAS_COROUTINES' /opt/facebook/folly/include/folly/
```

Alternatively, use `build/build_openr.sh` which handles the full dependency chain automatically.

See also: [Issue #135](https://github.com/facebook/openr/issues/135)

---

## 5. jemalloc Warning: `mallctl: not using jemalloc`

### Symptom

```
E Util.cpp:190] Failed to read thread allocated/de-allocated bytes:
mallctl: not using jemalloc
```

### Cause

This is a **non-fatal warning**. OpenR uses `folly::mallctlRead()` to sample per-thread allocation stats for monitoring. When the binary is linked against the system allocator (glibc `malloc`) instead of `jemalloc`, this call fails gracefully.

The only impact is that per-thread memory allocation metrics are unavailable in `fb303` counters. OpenR continues to function correctly.

### Fix (Optional)

If you want full memory telemetry, link OpenR against `jemalloc`:

```bash
apt-get install libjemalloc-dev
# Then rebuild with: cmake -DLINK_JEMALLOC=ON ...
```

See also: [Issue #134](https://github.com/facebook/openr/issues/134)

---

## 6. Python CLI: `ModuleNotFoundError`

### Symptom

```
ModuleNotFoundError: No module named 'openr.thrift'
```

### Cause

The `breeze` CLI depends on generated Thrift Python bindings that are not installed by the default `pip install py-openr` path. The Thrift stubs must be generated from the `.thrift` source files during build.

### Fix

```bash
# From the openr source root, generate and install Python Thrift bindings
cd openr/if
thrift1 --gen py:json,utf8strings -r OpenrCtrl.thrift
pip3 install -e openr/py/
```

Or use the full build path which handles this automatically:
```bash
bash build/build_openr.sh
```

See also: [Issue #72](https://github.com/facebook/openr/issues/72)

---

## 7. `breeze` CLI Crashes in Area-Aware Mode

### Symptom

```
KeyError: 'metric'
```

When running `breeze lm links` with OpenR configured in multi-area mode (`area_policies`).

### Cause

In area-aware mode, links in non-default areas use `adj_metric` from area policies rather than populating the top-level `metric` field. The `breeze lm links` display code accesses `link.metric` unconditionally.

### Workaround

Until a fix is merged, run `breeze lm adjacencies` instead to inspect link state — it displays the effective metric including area-policy overrides without hitting this crash.

See also: [Issue #163](https://github.com/facebook/openr/issues/163)

---

## 8. Collecting Diagnostic Information

When filing a bug report, please include the output of the following:

```bash
# OpenR version
openr --version

# OS and kernel
uname -a
lsb_release -a

# OpenR process status
systemctl status openr 2>/dev/null || supervisorctl status openr

# Neighbor state
breeze spark neighbors 2>/dev/null

# Adjacency table
breeze lm adjacencies 2>/dev/null

# KVStore key count
breeze kvstore keys 2>/dev/null | wc -l

# Recent OpenR logs (last 200 lines)
journalctl -u openr -n 200 --no-pager 2>/dev/null

# Interface state
ip link show
ip -6 addr show
```

Please also specify:
- Number of neighbors / interfaces
- Whether you are running in `.git` mode or native mode
- Whether `area_policies` are configured
- Any recent changes to OpenR config or the host network
