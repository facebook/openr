`fbmeshd`
---------------------

fbmeshd is an extension of OpenR for wireless meshes. fbmeshd is the core
technology behind Facebook's Self Organizing Mesh Access network.

fbmeshd uses 802.11s to form links with neighboring nodes in wireless range.
802.11s path selection and forwarding are disabled and OpenR is used for
routing over such links.

### How to run a fbmeshd powered WiFi mesh

fbmeshd is built using `CMake`. To run a fbmeshd mesh, you need to build
and install both vanilla OpenR and fbmeshd on a wireless device such as an Access Point.

fbmeshd replaces or adds a few modules that are used in vanilla OpenR. OpenR needs
to be started with the following flags for it use fbmeshd's modules.
```
--enable_spark=false --spark_report_url="tcp://[::1]:60019" --spark_cmd_url="tcp://[::1]:60018"
```

With this change, fbmeshd can be started using the command in `scripts/`
```
$ run_fbmeshd.sh
```

And then OpenR can be started to handle L3 routing.
