# OpenR Scale Test — Setup Scripts

Scripts for preparing a test server and an Arista EOS DUT to run the OpenR
scale test (`ScaleTestServer`). Both ends need matching VLAN sub-interfaces
and ip6tables rules that allow per-neighbor FakeKvStore Thrift traffic.

## Scripts

| Script              | Side        | Purpose                                                                |
| ------------------- | ----------- | ---------------------------------------------------------------------- |
| `setup_vlans.sh`     | Test server | Creates VLAN sub-interfaces, assigns IPs (`.1`), opens ip6tables.      |
| `setup_vlans_dut.sh` | EOS DUT     | Creates matching VLAN sub-interfaces, assigns IPs (`.2`), configures EOS via FastCli, opens ip6tables. |

Both must be run as root. Both are idempotent — re-running won't stack
duplicate ip6tables rules.

## Quick Start (4 VLANs)

On the test server:

```bash
sudo ./setup_vlans.sh eth0 4
# Creates eth0.1..eth0.4, IPv4 10.0.1.1/24..10.0.4.1/24, IPv6 fd00:0:0:1::1/64..
```

On the DUT (EOS — use the Linux interface name, e.g. `et4_15_1` for `Ethernet4/15/1`):

```bash
sudo ./setup_vlans_dut.sh et4_15_1 4
# Creates matching et4_15_1.1..et4_15_1.4, IPs end in .2
```

The setup_vlans.sh script prints an `--interfaces=` line you can paste into
`scale_test_server`:

```
Use with scale_test_server:
  --interfaces=eth0.1,eth0.2,eth0.3,eth0.4
```

## Larger Scale (e.g. 100 VLANs starting at ID 100)

```bash
sudo ./setup_vlans.sh     eth0 100 100   # server
sudo ./setup_vlans_dut.sh et4_15_1 100 100  # DUT
```

## Overriding ip6tables Port Ranges

Both scripts open ip6tables for the FakeKvStore Thrift servers spawned by
`ScaleTestServer` (one per simulated neighbor, starting at
`--fake_kvstore_base_port`, default `3000`).

Defaults:
- `INPUT_PORT_RANGE=3000:3500` (inbound to FakeKvStore servers)
- `OUTPUT_PORT_RANGE=3000:3100` (outbound replies)

Override when running with more neighbors or a different base port:

```bash
INPUT_PORT_RANGE=3000:3999 OUTPUT_PORT_RANGE=3000:3500 \
    sudo ./setup_vlans.sh eth0 800
```

## Cleanup

Each script prints a one-liner at the end to tear down the VLANs it created,
e.g.:

```bash
for i in $(seq 1 4); do sudo ip link delete eth0.$i; done
```

To remove the ip6tables rules:

```bash
sudo ip6tables -D INPUT  -p tcp --dport 3000:3500 -j ACCEPT
sudo ip6tables -D OUTPUT -p tcp --sport 3000:3100 -j ACCEPT
```
