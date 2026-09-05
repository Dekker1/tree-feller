#!/bin/sh
# One process per (mode, file), because peak RSS is a whole-process high-water
# mark. Files are given smallest first.
#
#   bench/run.sh <file>...
#
# `mzn_bench` is optional: it needs libminizinc built, and is skipped if the
# binary is not there.
set -u
BENCH=${BENCH:-./build/tf_bench}
MZN=${MZN:-./build/mzn_bench}

printf '%-11s %12s %10s %12s %12s  %s\n' MODE SIZE TIME THROUGHPUT PEAK_RSS FILE
for f in "$@"; do
  [ -f "$MZN" ] && for m in bison ts; do "$MZN" "$m" "$f" || echo "  (mzn_bench $m failed)"; done
  for m in ts-parse ts-walk feller feller-fold feller-named feller-raw feller-null; do
    "$BENCH" "$m" "$f" || echo "  (tf_bench $m failed)"
  done
done
