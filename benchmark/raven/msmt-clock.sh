#!/bin/sh

madcap=$1
outputdir=$2


protocol="$1"
if [ "$protocol" = "" ]; then
        echo "\"$0 {noencap|ipip|gre|gretap|vxlan|nsh} [OUTPUTDIR]\""
        exit
fi

outputdir=$2
if [ "$outputdir" = "" ]; then
        echo output to stdout
fi

ndgproc=/proc/driver/netdevgen
netdevgen=~/work/madcap/netdevgen/netdevgen.ko

ravenproc=/proc/driver/raven

count=300

sudo rmmod netdevgen
sudo insmod $netdevgen


file=$outputdir/$protocol.txt


if [ ! "$outputdir" = "" ]; then
	echo outputfile is $file
else
	echo output to stdout
fi

for n in `seq $count`; do
	echo xmit $t packet $n
	echo $protocol > $ndgproc 

	if [ ! "$outputdir" = "" ]; then
		cat $ravenproc \
		| tr -d ' ' | tr '\n' ' '|sed -e 's/$/\n/' >> $file
	else
		cat $ravenproc | tr -d ' ' | tr '\n' ' '|sed -e 's/$/\n/' 
	fi

	sleep 0.2
done
