### Encapsulation Madness!

- madcap/ contains madcap API. It is inspired by switchdev API. It provides `struct madcap_ops` for madcap capable device drivers. add/delete/dump overlay FIB are done through this API.
- iproute2-4.4.0/ is madcap capable ip command. see `ip madcap help`.
- device-drivers-4.2.0/ contains modified e1000 and ixgbe drivers. They are madcap API capable. The overlay FIB is implemented in {e1000|ixgbe}/sfmc.{c|h}.
- protocol-dirvers-3.19.0 or 4.2.0/ contains madcap capable ipip, gre, gretap, vxlan and nsh tunnel drivers.
- netdevgen/ is simple traffice generator working in kernel space. It is controlled via /proc/driver/netdevgen. benchmark scripts uses netdevgen.
- raven/ is dummy interface. you can create dummy interface via `ip link add type raven` command. packets transmitted to the raven device is dropped immediately. If the kernel is modified for benchmarking (http://github.com/upa/linux-madcap-msmt), raven shows the timestamp values of `strut sk_buff` that is transmitted via raven interface through /proc/driver/raven.
- benchmark/ contains benchmarking scripts. see benchmark/README.md

