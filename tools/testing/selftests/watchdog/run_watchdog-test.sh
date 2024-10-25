#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

echo "$0: Feed dog"
./watchdog-test -c 2 || exit

echo
echo "$0: Show watchdog_info"
./watchdog-test -i -c 2 || exit

echo
echo "$0 Turn off the watchdog timer"
./watchdog-test -d -c 2 || exit

echo
echo "$0: Turn on the watchdog timer"
./watchdog-test -e -c 2 || exit

echo
echo "$0: Set timeout to T seconds"
./watchdog-test -d -t 10 -c 2 -e || exit

echo
echo "$0: Get the timeout"
./watchdog-test -T -c 2 || exit

echo
echo "$0: Get the pretimeout to T seconds"
./watchdog-test -d -N -c 2 -e || exit

echo
echo "$0 Get the time left until timer expires"
./watchdog-test -L -c 2 || exit

echo
echo "Get status & supported features"
./watchdog-test -s || exit

echo
echo "$0: Set the pretimeout to T seconds"
./watchdog-test -n 3 -c 2 || exit
