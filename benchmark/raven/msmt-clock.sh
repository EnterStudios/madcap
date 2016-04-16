#!/bin/sh

madcap=$1
resultdir=$2

if [ "$resultdir" = "" ]; then
        echo "\"$0 {0|1} RESULTDIR\""
        exit
fi


ndgproc=/proc/driver/netdevgen
netdevgen=~/work/madcap/netdevgen/netdevgen.ko

ravenproc=/proc/driver/raven

count=200

sudo rmmod netdevgen
sudo insmod $netdevgen


for t in noencap ipip gre gretap vxlan nsh ; do

	file=$resultdir/$t.txt

	echo
	echo outputfile is $file
	echo
	sleep 1

	./setup-raven.sh $t $madcap
	sleep 1

	for n in `seq $count`; do
		echo xmit $t packet $n
		echo $t > $ndgproc 
		cat $ravenproc | tr -d ' ' | tr '\n' ' '|sed -e 's/$/\n/' >> $file
		#cat $ravenproc | tr -d ' ' | tr '\n' ' '|sed -e 's/$/\n/' 
		sleep 0.2
	done
done
