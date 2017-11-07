# How to Use SPP_VF

## SPP_VF

SPP_VF is a SR-IOV like network functionality for NFV.

![spp_vf_overview](spp_vf_overview.svg)

## Environment

* Ubuntu 16.04
* qemu-kvm
* DPDK v17.05 or later

## Launch SPP

Before launching spp, you need to setup described as [setup guide](setup_guide.md).

### SPP Controller

First, run SPP Controller with port numbers for spp primary and secondary.

```sh
$ python ./src/spp_vf.py -p 5555 -s 6666
```

### SPP Primary

SPP primary reserves and manages resources for secondary processes.
You have to run it before secondaries.

SPP primary has two kinds of options, dpdk and spp.
Option of dpdk is before `--`, option of spp is after `--`.

Option of dpdk are refer to [dpdk documentation](http://dpdk.org/doc/guides/linux_gsg/build_sample_apps.html#running-a-sample-application).

Options of spp primary are
  * -p : port mask
  * -n : number of rings
  * -s : ip addr and port of spp primary

```sh
$ sudo ./src/primary/x86_64-native-linuxapp-gcc/spp_primary \
-c 0x02 -n 4 --socket-mem 512,512 \
--huge-dir=/run/hugepages/kvm \
--proc-type=primary \
-- -p 0x03 -n 9 -s 127.0.0.1:5555
```

### SPP Secondary

In `spp_vf`, spp secondary processes are launched by single command.

`spp_vf` has two kinds of options as well as spp primary.

Option of dpdk are refer to [dpdk documentation](http://dpdk.org/doc/guides/linux_gsg/build_sample_apps.html#running-a-sample-application).

Options of `spp_vf` are
  * TODO

Core assingment and network configuration are defined
in JSON formatted config file.
If you run `spp_vf` without giving config file, it refers default
config file (test/spp_config/spp_config/vf.json).

```sh
$ sudo ./src/vf/x86_64-native-linuxapp-gcc/spp_vf \
-c 0x3ffd -n 4 --proc-type=secondary
```

You can also indicate which of config you use explicitly with
`--config` option as following example.
Please refer to sample config files in test/spp_config/spp_config.

[NOTE] Core mask should be changed to correspond with core assingment
defined in each of config files.

```sh
$ sudo ./src/vf/x86_64-native-linuxapp-gcc/spp_vf \
-c 0x3ffd -n 4 --proc-type=secondary \
-- --config /path/to/config/spp_vf1.json
```

### SPP VM

Launch VMs with `virsh` command.

```sh
$ virsh start [VM]
```

### Additional Network Configurations

To enable processes running on the VM to communicate through spp,
it is required additional network configurations on host and guest VMss.

#### Host1

```sh
# Interface for vhost
$ sudo ifconfig [IF_NAME] inet [IP_ADDR] netmask [NETMASK] up

# Disable offload for vhost interface
$ sudo ethtool -K [IF_NAME] tx off
```

#### Host2

```sh
# Disable offload for VM interface
$ ethtool -K [IF_NAME] tx off
```
