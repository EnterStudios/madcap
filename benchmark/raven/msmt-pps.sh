#!/bin/sh

outputdir=result-raven-pps-w-mc

interface=r0
ndgproc=/proc/driver/netdevgen

netdevgen=~/work/madcap/netdevgen/netdevgen.ko

pktgen_host=130.69.250.223
pktgen_path=/home/upa/work/netmap/examples/pkt-gen
pktgen_intf=p2p1
pktgen_cont=60

protocol="$1"
if [ "$protocol" = "" ]; then
        echo "\"$0 {noencap|ipip|gre|gretap|vxlan|nsh} [OUTPUTDIR]\""
        exit
fi

outputdir=$2
if [ "$outputdir" = "" ]; then
        echo output to stdout
fi

sudo rmmod netdevgen

for pktlen in 50 114 242 498 1010 1486; do

	sleep 1
	sudo insmod $netdevgen measure_pps=1 pktlen="$pktlen"
	sleep 1

	echo xmit $protocol packet, pktlen is $pktlen
	echo $protocol > $ndgproc
	sleep 2

	fpktlen=`expr $pktlen + 14`
	file=$outputdir/result-$fpktlen-$protocol.txt
	
	if [ ! "$outputdir" = "" ]; then
		echo outputfile is $file
	fi

	for n in `seq $pktgen_cont`; do
		if [ "$outputdir" = "" ]; then
			ifdata -pops $interface 
		else
			ifdata -pops $interface >> $file
		fi
	done

	echo stop > $ndgproc
	sleep 1
	sudo rmmod netdevgen

done
