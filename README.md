HyperV_Utilities
================

Contains HyperV Utilities Driver includes KVP Driver 

/* After cloning the FreeBSD source follow the below steps to build KVP utilities driver */ 

Create the below directories if they does not exist already
- mkdir /root/soc
- mkdir /var/opt/hyperv

How to compile kvp_client
- cd /usr/freebsd/sys/dev/hyperv/utilities
- make kvp_client

How to compile the user daemon code
- cd /usr/freebsd/sys/dev/hyperv/utilities
- make hv_kvp_daemon

How to setup the init scripts
- cd /usr/freebsd/sys/dev/hyperv/utilities
- cp hv_kvpd /etc/rc.d
- Edit /etc/rc.conf and include the below line:
hv_kvp_daemon_enable="YES"

How to compile the loadable kvp driver
- cd /usr/freebsd/sys/dev/hyperv/utilities
- make -f Makefile.ko hv_kvp.ko

How to compile the utils driver
- Edit /usr/freebsd/sys/conf/files to include hv_util.c and unicode.c
- cd /usr/freebsd
- make buildkernel KERNCONF=HYPERV_VM
- make installkernel KERNCONF=HYPERV_VM

- Reboot
