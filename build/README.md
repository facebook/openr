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

> NOTE: Whatever you do within docker container is lost once you logout
