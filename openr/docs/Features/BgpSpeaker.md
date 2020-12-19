# Bgp Speaker

Open/R implemented a plugin module to support BGP Protocol. This module can
establish a BGP session with external BGP daemon to import routes. This works
very similar as route redistribution but with the capability to get route
attributes information.

- There can only be one bgp peer configured in `bgp_config`.
- There's no policy support for this bgp session.

![Bgp Plugin](https://user-images.githubusercontent.com/5740745/102545893-1ce3b080-4074-11eb-82b6-5b46a58f5d18.png)
