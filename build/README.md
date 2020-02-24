`Build with fbcode_builder`
-------------------------------

Continuous integration builds are powered by `fbcode_builder`, a tiny tool
shared by several Facebook projects.  Its files are in `./fbcode_builder`
(on Github) or in `fbcode/opensource/fbcode_builder` (inside Facebook's
repo).

NOTE:
- libnl requires a patch included here with in build directory for correct
  handling of IPv6 multipath routes

### Step-0 Install Docker
---

```
apt install docker
```

### Step-1 Generate DockerConfig
---

```
// Get into build directory
cd openr/build

// Run following command
python fbcode_builder/make_docker_context.py

```

### Step-2 Build DockerImage with DockerConfig
---

```
// Get into tmp directory where DockerConfig is generated
cd /tmp/docker-context-m6h7_x

// Build and name image as `openr-build`
sudo docker build -t openr-build . | tee log
```

### Build on debian host without docker
---

The `debian_system_builder` helps you creating a build script using the `fbcode_builder` logic. The system builder extends the `shell_builder` to create a shell script that installs Open/R and its dependecies to the system. The `debian_system_builder` is tested on `ubuntu 16.04 server`.

To create and execute the build script run:

```
cd openr/build
python debian_system_builder/debian_system_builder.py > ./build_openr_debian.sh
sudo chmod +x build_openr_debian.sh
sudo ./build_openr_debian.sh
```

### Miscellaneous
---

If certain step in DockerConfig fails and you want to debug it then best way is
to comment out that broken step in DockerConfig (e.g. Open/R build step) and
build a docker container. This will get you a full docker context with all you
need.

Then you can get into docker container with following command and manually run
that broken command or iterate to address the issue

```
// `openr-build` is name of docker container
sudo docker run -u 0 -i -t openr-build /bin/bash
```
### Benchmarks
---

The benchmarking methodology is designed to help Open/R users to assess the functional efficiency of various modules of Open/R and identify time performance metrics. These metrics can be tracked to identify regressions and performance improvements.

We have created standalone benchmarks for ConfigStore, Fib, NetlinkFibHandler, Decision and KvStore. You can find the binary files from:

```
sbin/tests/openr/config-store/
sbin/tests/openr/fib/
sbin/tests/openr/platform/
sbin/tests/openr/decision/
sbin/tests/openr/kvstore/
```

> NOTE: Whatever you do within docker container is lost once you logout
