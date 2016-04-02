#!/bin/sh

outputdir=result-raven-pps-wo-mc

interface=r0
ndgproc=/proc/driver/netdevgen

netdevgen=~/work/madcap/netdevgen/netdevgen.ko

pktgen_host=130.69.250.223
pktgen_path=/home/upa/work/netmap/examples/pkt-gen
pktgen_intf=p2p1
pktgen_cont=70

protocol="$1"
if [ "$protocol" = "" ]; then
	echo "\"$0 {noencap|ipip|gre|gretap|vxlan|nsh}\""
	exit
fi


sudo rmmod netdevgen


for pktlen in 50 114 242 498 1010 1486; do
#for pktlen in 114 1010; do

	sleep 1
	sudo insmod $netdevgen measure_pps=1 pktlen="$pktlen"
	sleep 1

	echo xmit $protocol packet, pktlen is $pktlen
	echo $protocol > $ndgproc

	fpktlen=`expr $pktlen + 14`

	for n in `seq $pktgen_cont`; do
		ifdata -pops $interface >> \
		$outputdir/result-$fpktlen-$protocol.txt
		sleep 1
	done

	echo stop > $ndgproc
	sleep 1
	sudo rmmod netdevgen

done

