# OpenR: Open Routing

[![Build Status](https://github.com/facebook/openr/workflows/linux/badge.svg)](https://github.com/facebook/openr/actions?workflow=linux)

Open Routing, OpenR, is Facebook's internally designed and developed Interior Routing
Protocol/Platform. OpenR was originally designed and built for performing routing on the
[Terragraph](https://terragraph.com/) mesh network. OpenR's flexible design has led to
its adoption in other networks, including Facebook's new WAN network, Express Backbone.

## Documentation

---

Please refer to [`openr/docs/Overview.md`](openr/docs/Overview.md) to get
started with OpenR.

## Library Examples

---

Please refer to the [`examples`](examples) directory to see some useful ways to
leverage the openr and fbzmq libraries to build software to run with OpenR.

## Resources

---

* Developer Group: https://www.facebook.com/groups/openr/
* Github: https://github.com/facebook/openr/
* IRC: #openr on freenode

## Contribute

---

Take a look at [`openr/docs/DeveloperGuide.md`](openr/docs/DeveloperGuide.md)
and [`CONTRIBUTING.md`](CONTRIBUTING.md) to get started contributing.
The Developer Guide outlines best practices for code contribution and testing.
Any single change should be well tested for regressions and version
compatibility.

## Code of Conduct

---

The code of conduct is described in [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md)

## Requirements

---

We have tried `OpenR` on Ubuntu-16.04, Ubuntu-18.04 and CentOS 7/8.
OpenR should work on all Linux based platforms.

* Compiler supporting C++17 or higher
* libzmq-4.0.6 or greater

## Build

---

### Repo Directory Structure

At the top level of this repo are the `build` and `openr` directories. Under the
former is a tool, `gen`, that contains scripts for building the
project. The `openr` directory contains the source for the project.

### Dependencies

OpenR requires these dependencies for
your system and follows the traditional cmake build steps below.

* `cmake`
* `gflags`
* `gtest`
* `libsodium`
* `libzmq`
* `zstd`
* `folly`
* `fbthrift`
* `fbzmq`
* `re2-devel`

### One Step Build - Ubuntu

We've provided a script, `build/build_openr.sh`, tested on Ubuntu LTS releases.
It uses `gendeps.py` to install all necessary dependencies, compile OpenR and install
C++ binaries as well as python tools. Please modify the script as needed for
your platform. Also, note that some library dependencies require a newer version
than provided by the default package manager on the system and hence we are
compiling them from source instead of installing via the package manager. Please
see the script for those instances and the required versions.

### Build Steps

```console
# Install dependencies and openr
cd build
bash ./build_openr.sh

# To Run tests (some tests requires sudo privileges)
python3 build/fbcode_builder/getdeps.py test \
  --src-dir=. \
  --project-install-prefix openr:/opt/facebook \
  openr
```

If you make any changes you can run `cmake ../openr` and `make` from the build
directory to build openr with your changes.

### Installing

`openr` builds both static and dynamic libraries and the install step installs
libraries and all header files to `/opt/facebook/openr/lib` and
`/opt/facebook/openr/include/` along with python modules in your Python's
`site-packages` directory.
Note: the `build_openr.sh` script will run this step for you

* Manually you can drive `getdeps.py` to install elsewhere
  * refer to `build_openr.sh`


#### Installing Python Libraries

You will need python `pip` or `setuptools` to build and install python modules.
All library dependencies will be automatically installed except the
`fbthrift-python` module which you will need to install manually using steps
similar to those described below. This will install `breeze`, a cli tool to
interact with OpenR.

* Python install requires a `fbthrift` / `thrift1` compiler to be installed and in PATH

```console
cd openr/openr/py
python setup.py build
sudo python setup.py install
```

### Docker Building / Usage

OpenR now has a `Dockerfile`. It uses `gendeps.py` to build all dependencies + OpenR.
It also installs the OpenR CLI `breeze` into the container.

```console
 docker build --network host .
```

#### Running

You can specify a config file by bind mount a directory with a `openr.cfg` file in /config

```console
docker run --name openr --network host openr_ubuntu
```

- To use a custom config bind mount `/config` into the container
  - OpenR binary will look for `/config/openr.conf`

### License

OpenR is [MIT licensed](./LICENSE).
