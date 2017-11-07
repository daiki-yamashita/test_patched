# Setup Guide

## Environment

* Ubuntu 16.04
* qemu-kvm 2.7 or later
* DPDK v17.05 or later

## Setting

### Host

#### Edit Config

Uncomment user and group in `/etc/libvirt/qemu.conf`.

```sh
# /etc/libvirt/qemu.conf

user = "root"
group = "root"
```

Change `KVM_HUGEPAGES` from 0 to 1 in `/etc/default/qemu-kvm`.

```sh
# /etc/default/qemu-kvm

KVM_HUGEPAGES=1
```

Change grub config for hugepages and isolcpus.

```sh
# /etc/default/grub

GRUB_CMDLINE_LINUX_DEFAULT="isolcpus=2,4,6,8,10,12-18,20,22,24,26-42,44,46 hugepagesz=1G hugepages=36 default_hugepagesz=1G"
```

You need to run `update-grub` and reboot to activate grub config.

```sh
$ sudo upadte-grub
$ sudo reboot
```

You can check hugepage settings as following.

```sh
$ cat /proc/meminfo | grep -i huge
AnonHugePages:      2048 kB
HugePages_Total:      36		#	/etc/default/grub
HugePages_Free:       36
HugePages_Rsvd:        0
HugePages_Surp:        0
Hugepagesize:    1048576 kB		#	/etc/default/grub

$ mount | grep -i huge
cgroup on /sys/fs/cgroup/hugetlb type cgroup (rw,nosuid,nodev,noexec,relatime,hugetlb,release_agent=/run/cgmanager/agents/cgm-release-agent.hugetlb,nsroot=/)
hugetlbfs on /dev/hugepages type hugetlbfs (rw,relatime)
hugetlbfs-kvm on /run/hugepages/kvm type hugetlbfs (rw,relatime,mode=775,gid=117)
hugetlb on /run/lxcfs/controllers/hugetlb type cgroup (rw,relatime,hugetlb,release_agent=/run/cgmanager/agents/cgm-release-agent.hugetlb,nsroot=/)
```

Finally, you unmount default hugepage.

```sh
$ sudo unmount /dev/hugepages
```

#### Install jasson

Network configuration is defined in JSON and `spp_vf` reads config from
the file while launching.
[jasson](http://www.digip.org/jansson/) is a JSON library written in C.

Install the -dev package.

```sh
$ sudo apt-get install libjansson-dev
```

#### Install DPDK

Install DPDK in any directory. This is a simple instruction and please refer
[Getting Started Guide for Linux](http://dpdk.org/doc/guides/linux_gsg/index.html)
for details.

```sh
$ cd /path/to/any_dir
$ git clone http://dpdk.org/git/dpdk
$ cd dpdk
$ git checkout [TAG_NAME(e.g. v17.05)]
$ export RTE_SDK=`pwd`
$ export RTE_TARGET=x86_64-native-linuxapp-gcc
$ make T=x86_64-native-linuxapp-gcc install
```

#### Install SPP

Clone SPP in any directory and compile it.

```sh
$ cd /path/to/spp_home/
$ git clone https://github.com/ntt-ns/Soft-Patch-Panel.git
export SPP_HOME=/path/to/spp_home/Soft-Patch-Panel
$ cd $SPP_HOME
$ make
```

#### Setup for DPDK

Load igb_uio module.

```sh
$ sudo modprobe uio
$ sudo insmod $RTE_SDK/x86_64-native-linuxapp-gcc/kmod/igb_uio.ko
$ lsmod | grep uio
igb_uio                16384  0  # igb_uio is loaded
uio                    20480  1 igb_uio
```

Then, bind it with PCI_Number.
```sh
$ $RTE_SDK/usertools/dpdk-devbind.py --status
# check your device for PCI_Number

$ sudo $RTE_SDK/usertools/dpdk-devbind.py --bind=igb_uio [PCI_Number]
```

#### virsh setup

Edit VM configuration with virsh.

```sh
$ virsh edit [VM_NAME]
```

```xml
<domain type='kvm' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>
	  <name>spp-vm1</name>
	  <uuid>d90f5420-861a-4479-8559-62d7a1545cb9</uuid>
	  <memory unit='KiB'>4194304</memory>
	  <currentMemory unit='KiB'>4194304</currentMemory>
	  <memoryBacking>
	    <hugepages/>
	  </memoryBacking>
	  <vcpu placement='static'>4</vcpu>
	  <os>
	    <type arch='x86_64' machine='pc-i440fx-2.3'>hvm</type>
	    <boot dev='hd'/>
	  </os>
	  <features>
	    <acpi/>
	    <apic/>
	    <pae/>
	  </features>
	  <clock offset='utc'/>
	  <on_poweroff>destroy</on_poweroff>
	  <on_reboot>restart</on_reboot>
	  <on_crash>restart</on_crash>
	  <devices>
	    <emulator>/usr/local/bin/qemu-system-x86_64</emulator>
	    <disk type='file' device='disk'>
	      <driver name='qemu' type='raw'/>
	      <source file='/var/lib/libvirt/images/spp-vm1.qcow2'/>
	      <target dev='hda' bus='ide'/>
	      <address type='drive' controller='0' bus='0' target='0' unit='0'/>
	    </disk>
	    <disk type='block' device='cdrom'>
	      <driver name='qemu' type='raw'/>
	      <target dev='hdc' bus='ide'/>
	      <readonly/>
	      <address type='drive' controller='0' bus='1' target='0' unit='0'/>
	    </disk>
	    <controller type='usb' index='0'>
	      <address type='pci' domain='0x0000' bus='0x00' slot='0x01' function='0x2'/>
	    </controller>
	    <controller type='pci' index='0' model='pci-root'/>
	    <controller type='ide' index='0'>
	      <address type='pci' domain='0x0000' bus='0x00' slot='0x01' function='0x1'/>
	    </controller>
	    <interface type='network'>
	      <mac address='52:54:00:99:aa:7f'/>
	      <source network='default'/>
	      <model type='rtl8139'/>
	      <address type='pci' domain='0x0000' bus='0x00' slot='0x02' function='0x0'/>
	    </interface>
	    <serial type='pty'>
	      <target type='isa-serial' port='0'/>
	    </serial>
	    <console type='pty'>
	      <target type='serial' port='0'/>
	    </console>
	    <memballoon model='virtio'>
	      <address type='pci' domain='0x0000' bus='0x00' slot='0x03' function='0x0'/>
	    </memballoon>
	  </devices>
	  <qemu:commandline>
	    <qemu:arg value='-cpu'/>
	    <qemu:arg value='host'/>
	    <qemu:arg value='-object'/>
	    <qemu:arg value='memory-backend-file,id=mem,size=4096M,mem-path=/run/hugepages/kvm,share=on'/>
	    <qemu:arg value='-numa'/>
	    <qemu:arg value='node,memdev=mem'/>
	    <qemu:arg value='-mem-prealloc'/>
	    <qemu:arg value='-chardev'/>
	    <qemu:arg value='socket,id=chr0,path=/tmp/sock0,server'/>
	    <qemu:arg value='-device'/>
	    <qemu:arg value='virtio-net-pci,netdev=vhost-net0,mac=52:54:00:12:34:56'/>
	    <qemu:arg value='-netdev'/>
	    <qemu:arg value='vhost-user,id=vhost-net0,chardev=chr0,vhostforce'/>
	    <qemu:arg value='-chardev'/>
	    <qemu:arg value='socket,id=chr1,path=/tmp/sock1,server'/>
	    <qemu:arg value='-device'/>
	    <qemu:arg value='virtio-net-pci,netdev=vhost-net1,mac=52:54:00:12:34:57'/>
	    <qemu:arg value='-netdev'/>
	    <qemu:arg value='vhost-user,id=vhost-net1,chardev=chr1,vhostforce'/>
	  </qemu:commandline>
	</domain>
```

### Trouble Shooting

You might encounter a permission error for `tmp/sockN` because of appamor.
In this case, you should try it.

```sh
$ sudo ln -s /etc/apparmor.d/usr.lib.libvirt.virt-aa-helper /etc/apparmor.d/disable/usr.lib.libvirt.virt-aa-helper
$ sudo ln -s /etc/apparmor.d/usr.sbin.libvirtd /etc/apparmor.d/disable/usr.sbin.libvirtd
$ sudo apparmor_parser -R /etc/apparmor.d/usr.lib.libvirt.virt-aa-helper
$ sudo apparmor_parser -R /etc/apparmor.d/usr.sbin.libvirtd
$ sudo service apparmor reload
$ sudo service apparmor restart
$ sudo service libvirt-bin restart
```

Or, you remove appamor.

```sh
$ sudo apt-get remove apparmor
```

If you use CentOS, not Ubuntu, confirm that SELinux doesn't prevent for permission.
SELinux should be disabled in this case.

```sh
# /etc/selinux/config
SELINUX=disabled
```

Check your SELinux configuration.

```sh
$ getenforce
Disabled
```
