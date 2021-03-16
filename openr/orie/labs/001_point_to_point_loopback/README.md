# ORIE Lab 001

This lab we bring up two Network Namespaces and share routes to out loopback
prefixes.

## Setup

### Namespaces

We have the `setup_openr_lab001.sh` helper script. It can:

- `create` namespaces
  - `sudo ./setup_openr_lab001.sh create`
- `check` namespaces
  - Show interface addrs + routes
  - `sudo ./setup_openr_lab001.sh check`
- `delete` namespaces
  - `sudo ./setup_openr_lab001.sh delete`
- `copy-conf` to /tmp
  - `./setup_openr_lab001.sh copy-conf`
- `start` Open/R
  - Copy config from fbcode to /tmp to get around Eden issues
  - **Run from within the namespaces**
  - `./setup_openr_lab001.sh start`

### Build Open/R

- `buck build @mode/opt openr:openr`

_(You could also use fbpkg to pull latest production verison too)_

## Run Open/R

Use screen or tmux and run a openr in each namespace.

- `./setup_openr_lab001.sh start`
