#!/usr/bin/env bash

for pars in 1000000:10000 100000:100000 10000:1000000 1000:3000000 100:3000000 10:3000000 1:3000000 0:3000000 ; do
	nsec=${pars%:*}
	sigs=${pars#*:}
	gcc-6 -l czmq -l zmq -o zmq-signals-speed -D USE_PIPE -D WAIT_NSEC=$nsec -D MAX_SIGS=$sigs zmq-signals-speed.c 
	for i in {1..3} ; do
		./zmq-signals-speed || exit $?
	done
	gcc-6 -l czmq -l zmq -o zmq-signals-speed -D WAIT_NSEC=$nsec -D MAX_SIGS=$sigs zmq-signals-speed.c 
	for i in {1..3} ; do
		./zmq-signals-speed || exit $?
	done
done
