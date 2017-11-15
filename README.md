`OpenR: Open Routing`
---------------------

Open Routing, OpenR, is Facebook's internally designed and developed routing
protocol/platform. Primarily built for performing routing on `Terragraph`
network but it's awesome design and flexibility has lead to it's adoption onto
other networks as well including WAN network (Express Backbone).

### Documentation
---

Please refer to [`openr/docs/Overview.md`](openr/docs/Overview.md) to get
starting with OpenR.

### Library Examples
---

Please refer to the [`examples`](examples) directory to see some useful ways to
leverage the openr and fbzmq libraries to build software to run with OpenR.

### Resources
---

* Developer Group: https://www.facebook.com/groups/openr/
* Github: https://github.com/facebook/openr/

### Contribute
---

Take a look at [`openr/docs/DeveloperGuide.md`](openr/docs/DeveloperGuide.md)
and [`CONTRIBUTING.md`](CONTRIBUTING.md) to get started with contribution.
DeveloperGuide outlines best practices for code contribution and testing. Any
single change should be well tested against regressions and version
compatibility.

### Requirements
---

We have tried `OpenR` on Ubuntu-14.04, Ubuntu-14.04, Ubuntu-16.04 and CentOS-7.
This should work on all Linux based platforms without any issues.

* Compiler supporting C++14 or higher
* libzmq-4.0.6 or greater


### Build
---
#### Repo Directory Structure

At the top level of this repo are the `build` and `openr` directories. Under the
former is a tool, `fbcode_builder`, that contains scripts for generating a
docker context to build the project. Under the `openr` directory exists the
source for the project.

#### Build using docker

On a docker enabled machine, from the top level of this repo simply run:
`./build/fbcode_builder/travis_docker_build.sh`

#### Dependencies

If docker is not a good option for you, you can install these dependencies for
your system and follow the traditional cmake build steps below.

* `cmake`
* `gflags`
* `gtest`
* `libsodium`
* `libzmq`
* `zstd`
* `folly`
* `fbthrift`
* `fbzmq`
* `libnl`

#### One Step Build - Ubuntu-16.04

We've provided a script, `openr/build/build_openr.sh`, well tested on
Ubuntu-16.04, to install all necessary dependencies, compile OpenR and install
C++ binaries as well as python tools. Please modify the script as needed for
your platform. Also note that some library dependencies require a newer version
than provided by the default package manager on system and hence we are
compiling them from source instead of installing via package manager. Please
see the script for those instances and the required version.

> `libnl` also requires custom patch, included herewith, for correct handling
of add/remove multicast routes.

#### Build Steps

```
// Step into `build` directory
cd openr/build

// Install dependencies and openr
sudo bash build_openr.sh

// Run tests (some tests requires sudo previledges)
sudo make test
```

If you make any changes you can run `cmake ../openr` and `make` from build
directory to build openr along with your changes.

#### Installing
`openr` builds a static as well as dynamic library and install steps installs
library as well all header files to `/usr/local/lib/` and `/usr/local/include/`
(under openr sub directory) along with python modules in `site-packages`.
Already installed by `build_openr.sh` script

```
sudo make install
```

#### Installing Python Libraries

You will need python `setup-tools` to build and install python modules. All
library dependencies will be automatically installed except the
`fbthrift-python` which you will need to install manually using the similar to
described below. This will install two tools, `breeze` a cli tool to interact
with OpenR and `emulator` a cli tool for emulating (and testing openr) in
virtual network topologies.

```
cd openr/openr/py
python setup.py build
sudo python setup.py install
```

Also install following tools to make emulator functional on hosts where you want
to run your emulation.
- `sshd` (SSH daemon)
- `screen`
- `fping`

### License
---

OpenR is MIT licensed.
