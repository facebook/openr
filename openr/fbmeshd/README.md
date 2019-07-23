`fbmeshd`
---------------------

fbmeshd is an extension of OpenR for wireless meshes. fbmeshd is the core
technology behind Facebook's Self-organizing Mesh Access network.

fbmeshd uses 802.11s to form links with neighboring nodes in wireless range.
802.11s path selection and forwarding are disabled and the OpenR Wireless Mesh
Routing (OWMP) protocol is used for routing over such links.

### How to run an fbmeshd-powered WiFi mesh

fbmeshd is built using `CMake`. To run an fbmeshd mesh, you need to build and
install fbmeshd on a wireless device such as an access point.

fbmeshd can be started using the command in `scripts/`
```
$ run_fbmeshd.sh
```
which will cause it to form L2 links and handle L3 routing.
