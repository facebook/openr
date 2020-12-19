# Area

Open/R can be divided into sub-domains called areas. An area is a logical
collection of routers and links. All routers within the same area maintain the
same KvStore database. A router does not have detailed information about
information outside its configured area, which thereby reduces the size of its
KvStore database.

- A link can only belong to one area.
- A router can belong to multiple areas. "Area Border Routers" refers to routers
  with multiple area configured.

![OpenrArea](https://user-images.githubusercontent.com/5740745/102544321-d68d5200-4071-11eb-87fd-87356a12be7a.png)
