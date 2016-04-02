#!/bin/sh
# (re)set up modified protocol drivers with ixgbe

s=sudo
ip=~/work/madcap/iproute2-4.4.0/ip/ip

protocol=$1

mc="$2"
if [ "$mc" = "" ]; then
	echo \"$0 [PROTO] 1\" is madcap TX path mode.
	echo \"$0 [PROTO] 0\" is native TX path mode.
	exit
fi

madcap_mode="madcap_enable=$mc"

dev=r0

madcap=~/work/madcap/madcap/madcap.ko
raven=~/work/madcap/raven/raven.ko
ipip=~/work/madcap/protocol-drivers-4.2.0/ipip/ipip.ko
gre=~/work/madcap/protocol-drivers-4.2.0/gre/ip_gre.ko
vxlan=~/work/madcap/protocol-drivers-4.2.0/vxlan/vxlan.ko
nsh=~/work/madcap/protocol-drivers-4.2.0/nsh/nshkmod.ko

ipnsh=~/work/madcap/protocol-drivers-4.2.0/nsh/iproute2-3.19.0/ip/ip

raw_srcip=172.16.6.1
raw_dstip=172.16.6.2
raw_dstmac=a0:36:9f:15:84:fc

ipip_srcip=172.16.1.1
ipip_dstip=172.16.1.2

gre_srcip=172.16.2.1
gre_dstip=172.16.2.2

gretap_srcip=172.16.3.1
gretap_dstip=172.16.3.2
gretap_dstmac=7a:a3:28:27:a3:aa

vxlan_srcip=172.16.4.1
vxlan_dstip=172.16.4.2
vxlan_dstmac=7a:a3:28:27:a3:a9

nsh_srcip=172.16.5.1
nsh_dstip=172.16.5.2
nsh_dstmac=7a:a3:28:27:a3:ab

echo unload modules.

$s rmmod ixgbe # ixgbe depends on vxlan.

$s rmmod ipip
$s rmmod ip_gre
$s rmmod gre
$s rmmod vxlan
$s rmmod nshkmod

$s rmmod raven
$s rmmod madcap

$s rmmod ip_tunnel
$s rmmod udp_tunnel

$s rmmod raven


echo load madcap and raven

$s modprobe udp_tunnel
$s modprobe ip6_udp_tunnel
$s insmod $madcap
$s insmod $vxlan $madcap_mode	# ixgbe needs vxlan
$s insmod $raven $madcap_mode drop_mode=1

echo setup raven $dev
$s ip link add name r0 type raven
$s ifconfig $dev up
$s ifconfig $dev mtu 9216
$s ifconfig $dev $raw_srcip/24
$s ip route add to $raw_dstip via $raw_dstip dev $dev
$s arp -s $raw_dstip $raw_dstmac


case $protocol in
	noencap)
	echo noencap mode.
	echo nothing to do. done.
	;;

	ipip)
	echo ipip mode.
	echo load modules
	$s modprobe ip_tunnel
	$s modprobe tunnel4
	$s insmod $ipip $madcap_mode

	echo setup madcap
	$s $ip madcap set dev $dev offset 0 length 0 proto ipip src $raw_srcip
	$s $ip madcap add dev $dev id 0 dst $raw_dstip

	echo setup ipip dev
	$s ip tunnel add ipip1 mode ipip remote $raw_dstip local $raw_srcip \
	dev $dev
	$s ifconfig ipip1 up
	$s ifconfig ipip1 $ipip_srcip/24

	;;

	gre)
	echo gre mode.
	echo load modules
	$s modprobe ip_tunnel
	$s modprobe tunnel4 
	$s modprobe gre
	$s insmod $gre $madcap_mode

	echo setup madcap
	$s $ip madcap set dev $dev offset 0 length 0 proto gre src $raw_srcip
	$s $ip madcap add dev $dev id 0 dst $raw_dstip

	echo setup gre dev
	$s ip tunnel add gre1 mode gre remote $raw_dstip local $raw_srcip \
	dev $dev
	$s ifconfig gre1 up
	$s ifconfig gre1 $gre_srcip/24

	;;

	gretap)
	echo gretap mode.
	echo load modules
	$s modprobe ip_tunnel 
	$s modprobe tunnel4 
	$s modprobe gre
	$s insmod $gre $madcap_mode

	echo setup madcap
	$s $ip madcap set dev $dev offset 0 length 0 proto gre src $raw_srcip
	$s $ip madcap add dev $dev id 0 dst $raw_dstip

	echo setup gretap dev
	$s ip link add gretap1 type gretap local $raw_srcip remote $raw_dstip \
	dev $dev
	$s ifconfig gretap1 up
	$s ifconfig gretap1 $gretap_srcip/24
	$s arp -s $gretap_dstip $gretap_dstmac

	;;

	vxlan)
	echo vxlan mode.
	echo vxlan module is already loaded for ixgbe.

	echo setup madcap
	$s $ip madcap set dev $dev offset 8 length 48 proto udp src $raw_srcip
	$s $ip madcap set dev $dev udp enable dst-port 4789 src-port 4789
	$s $ip madcap add dev $dev id 0 dst $raw_dstip

	echo setup vxlan dev
	$s ip link add type vxlan local $raw_srcip remote $raw_dstip id 0 \
	dev $dev
	$s ifconfig vxlan0 up
	$s ifconfig vxlan0 $vxlan_srcip/24
	$s arp -s $vxlan_dstip $vxlan_dstmac

	;;

	nsh)
	echo nsh mode.
	echo load modules
	$s modprobe udp_tunnel
	$s insmod $nsh $madcap_mode

	echo setup madcap
	$s $ip madcap set dev $dev offset 12 length 32 proto udp src $raw_srcip
	$s $ip madcap set dev $dev udp enable dst-port 4790 src-port 4790
	$s $ip madcap add dev $dev id 0 dst $raw_dstip

	echo setup nsh dev
	$s $ipnsh link add type nsh spi 10 si 5 \
	remote $raw_dstip local $raw_srcip vni 0 dev $dev
	$s $ipnsh nsh add spi 10 si 5 encap vxlan \
	remote $raw_dstip local $raw_srcip vni 0 dev $dev
	$s ifconfig nsh0 up
	$s ifconfig nsh0 $nsh_srcip/24
	$s arp -s $nsh_dstip $nsh_dstmac

	;;
esac
