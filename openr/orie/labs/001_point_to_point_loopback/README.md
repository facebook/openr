# ORIE Lab 001

This lab we bring up two Network Namespaces and share routes to out loopback
prefixes.

## Setup

### Namespaces

We use [json2netns](https://github.com/cooperlees/json2netns) for netns
creation/cleanup and `orie_helper.sh`to start Open/R and copy configs.

#### Internal build

- `buck build @mode/opt openr:openr python/wheel/json2netns/bin:json2netns`

#### Create Namespaces

- `create` namespaces
  - `sudo json2netns orie/labs/001_point_to_point_loopback/orie_001.json create`
- `check` namespaces
  - Show interface addrs + routes
  - `sudo json2netns orie/labs/001_point_to_point_loopback/orie_001.json check`
- `delete` namespaces
  - `sudo json2netns orie/labs/001_point_to_point_loopback/orie_001.json delete`
- `copy-conf` to /tmp (really internal only)
  - `orie_helper.sh copy-conf`

## Run Open/R

Use screen or tmux and run a openr in each namespace.

- `ip netns exec NETNS_NAME bash`
- `start` Open/R
  - Copy config from repo to /tmp/orie_configs to make config discovery easier
    - And this gets around a internal repo issue + netns
  - **Run from within the namespaces**
  - `orie_helper.sh start`
