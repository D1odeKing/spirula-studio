#!/bin/bash
# what was left at the pause: sift_prog (reuses its own features/matches in place),
# hybrid_extreme, then hybrid at ratio 0.95 and 1.0
cd /h/3DGS-Tools/Spirula-Studio-Dev/hybrid_tests
IMG="H:/Hershy Site GS/GS runs/a7iii_inside/images"
run() { name=$1; bin=$2; shift 2; s=$(date +%s);
  echo "$bin sfm auto \"$IMG\" -o ws_$name --manifest manifest.yaml $*" > ws_$name/cmd.txt
  "$bin" sfm auto "$IMG" -o ws_$name --manifest manifest.yaml "$@" > ws_$name/run.log 2>&1
  echo "$name exit=$? seconds=$(( $(date +%s) - s ))" >> queue.txt; }
echo "resumed $(date)" >> queue.txt
mv ws_sift_prog/run.log ws_sift_prog/run_paused.log 2>/dev/null
run sift_prog bin_base/spirula.exe --quality high --features sift --progressive
mkdir -p ws_hybrid_extreme; run hybrid_extreme bin_work1/spirula.exe --quality extreme --features loma-b128 --matcher loma-b128 --progressive --hybrid-sift
LOMA="--quality high --features loma-b128 --matcher loma-b128 --progressive --hybrid-sift --reuse-matches keep"
for r in 0.95 1.0; do n=hybrid_ratio${r/./}; mkdir -p ws_$n; cp -a ws_hybrid/features ws_hybrid/matches.bin ws_hybrid/.resume ws_$n/
  SS_SFM_HYBRID_RATIO=$r run $n bin_work1/spirula.exe $LOMA; done
echo "queue done $(date)" >> queue.txt
