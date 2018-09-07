#!/bin/sh
screen -S dumpinput.ratma.net -dm
# workaround for https://savannah.gnu.org/bugs/index.php?54164
sleep 1
screen -S dumpinput.ratma.net -X stuff "/srv/http/dumpinput.ratma.net/a.out 0.0.0.0 80^M"

