#!/bin/bash
killall pcp-server
sleep 0.1
pcp-server -r 0 --ear 1 && pcp-server --ip ::1 -r 3 --ear 1 &
sleep 1
$VALGRIND test_flow_notify
