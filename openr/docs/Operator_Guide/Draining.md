# Draining

`Draining` is a common term used in the computer networking world to describe
asking a network router to withdraw advertisement of prefixes it knows how to
get to and does not exist on itself (e.g. loopback prefixes).

This casues the network device to no longer injest traffic not destined to
itself. Draining a device is used when we wish to do maintanence work or take it
out of the "data path" if an operator suspects it's causing issues (e.g.
dropping packets).

Open/R has a few ways to be put in different classes of a "drained state". Here
we attempt to explain them.

## Drain a running Open/R Device

Open/R has thrift endpoints to ask the daemon to drain itself. Open/R supports
draining a link and whole device/node. Using the `breeze` CLI is an option to
hit these endpoints and see/control the drain state.

### See Drain Status

- `breeze openr interfaces`
  - alias of `breeze lm links`

### Drain

- A link
  - `breeze lm set-link-overload INTERFACE`
- A device/node
  - `breeze lm set-node-overload`

### Undrain

- A link
  - `breeze lm unset-link-overload INTERFACE`
- A device/node
  - `breeze lm unset-node-overload`

#### API

If you wish to write you own tool, just look at the `breeze` commands source
code to workout the thrift calls used and the thrift files to inspect other API
options avaliable.

## Drain startup flow

Here is the order of evaluation Open/R performs to work out if it should be in a
"drained" state or not:

1. Look for a platform specific "I am undrained" file on the file system
   configured by `undrained_flag_path`

- e.g. for FBOSS we look at `/dev/shm/fboss/UNDRAINED`

2. If set, look for a `persistent_config_store_path` file and load the state
   from that file

- This file is written out on clean Open/R shutdowns

3. We fallback to asumming drained if the config / CLI option `assumed_drained`
   is set or not

## Configuration File Options

The config file has options to tune draining behavior. The CLI options default
from the configuration file. The configuration file is the preferred way to
configure Open/R.

- `assume_drained`: Bool of wether or not to
- `persistent_config_store_path`: File path of where to stire persistent
  configuration data - e.g. Overload state
- `undrained_flag_path`: File path of a file to look for to unset node overload
  bit at startup

### CLI Arguments

Open/R has the following CLI/configuration options to control drain behavior:

- `-override_drain_state`: override persistent store drain state with value
  passed in `assume_drained` configuration setting
