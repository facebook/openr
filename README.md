

`OpenR: Open Routing`
---------------------

[![Build Status](https://travis-ci.org/facebook/openr.svg?branch=master)](https://travis-ci.org/facebook/openr)

Open Routing, OpenR, is Facebook's internally designed and developed routing
protocol/platform. Originally built for performing routing on the `Terragraph`
network, its awesome design and flexibility have led to its adoption in
other networks at Facebook including our new WAN network, Express Backbone.

### Documentation
---

Please refer to [`openr/docs/Overview.md`](openr/docs/Overview.md) to get
started with OpenR.

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
and [`CONTRIBUTING.md`](CONTRIBUTING.md) to get started contributing.
The Developer Guide outlines best practices for code contribution and testing.
Any single change should be well tested for regressions and version
compatibility.

### Code of Conduct
---

The code of conduct is described in [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md)

### Requirements
---

We have tried `OpenR` on Ubuntu-14.04, Ubuntu-16.04 and CentOS-7.
This should work on all Linux based platforms without any issues.

* Compiler supporting C++14 or higher
* libzmq-4.0.6 or greater


### Build
---
#### Repo Directory Structure

At the top level of this repo are the `build` and `openr` directories. Under the
former is a tool, `fbcode_builder`, that contains scripts for generating a
docker context to build the project. The `openr` directory contains the
source for the project.

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

We've provided a script, `build/build_openr.sh`, well tested on
Ubuntu-16.04, to install all necessary dependencies, compile OpenR and install
C++ binaries as well as python tools. Please modify the script as needed for
your platform. Also, note that some library dependencies require a newer version
than provided by the default package manager on the system and hence we are
compiling them from source instead of installing via the package manager. Please
see the script for those instances and the required versions.

> `libnl` also requires custom patch, included herewith, for correct handling
of add/remove multicast routes.

#### Build using Docker

Learn more [here.](https://github.com/facebook/openr/blob/master/build/fbcode_builder/README.md)

#### Build Steps

```
// Step into `build` directory
cd build

// Install dependencies and openr
sudo bash ./build_openr.sh

// Run tests (some tests requires sudo privileges)
sudo make test
```

If you make any changes you can run `cmake ../openr` and `make` from the build
directory to build openr with your changes.

#### Installing
`openr` builds both static and dynamic libraries and the install step installs
libraries and all header files to `/usr/local/lib/` and `/usr/local/include/`
(under openr subdirectory) along with python modules in `site-packages`.
Note: the `build_openr.sh` script will run this step for you.

```
sudo make install
```

#### Installing Python Libraries

You will need python `setup-tools` to build and install python modules. All
library dependencies will be automatically installed except the
`fbthrift-python` module which you will need to install manually using steps
similar to those described below. This will install `breeze`, a cli tool to
interact with OpenR.

```
cd openr/openr/py
python setup.py build
sudo python setup.py install
```

### License
---

OpenR is MIT licensed.
