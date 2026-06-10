#!/bin/sh
cd /Volumes/Home/nigelshearman/Development/amiga/A2065/tests/lance_logs
while a=1
do
name=`ls -latr run* | tail -1 | awk '{$9 = "\*"substr($9, 8); print $9}'`
egrep 'FAIL|WARN|Internal loopback test.......... PASS' $name | cut -c 30- | sort | uniq -c | sort
run=`ls -latr run* | tail -1 | awk '{$9 = substr($9, 4, 3); print $9}'`
echo "Tests run = "$run
echo "----------"; sleep 120
done
