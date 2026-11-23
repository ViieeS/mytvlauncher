#!/bin/sh
# Runs on the TV against a debug launcher (make DEBUG=1) that is in front, and prints the log lines
# of this run: three `bench` key walks, a pointer sweep across the dock and a wiggle inside its
# middle tile. perf_gaps.py reads the output. The pointer points assume the dock at the bottom of a
# 1920x1080 screen (tiles centred on y=916, the middle one on x=960).
C=/tmp/launcher_cmd
LOG=/tmp/launcher.log
n=$(date +%s)
cmd() { n=$((n + 1)); echo "$n $1" > $C; usleep $(( ${2:-300} * 1000 )); }   # busybox sleep takes whole seconds

start=$(wc -l < $LOG)
cmd rest 1000
cmd bench 13000
cmd bench 13000
cmd bench 13000

cmd rest 1000
cmd statbeg
for x in 340 380 420 460 500 540 580 620 660 700 740 780 820 860 900 940 980 1020 1060 1100 \
         1140 1180 1220 1260 1300 1340 1380 1420 1460 1500 1540 1580; do
  cmd "hover $x 916"
done
cmd statend 1000

cmd statbeg
for p in "880 870" "1040 870" "1040 960" "880 960" "960 916" "900 880" "1020 950" "930 900" \
         "990 930" "880 916" "1040 916" "960 870" "960 960" "960 916"; do
  cmd "hover $p" 450
done
cmd statend 1000
cmd rest 1000
tail -n +$((start + 1)) $LOG | grep -E '^present |^cmd [0-9]+ (bench|statbeg)$'
