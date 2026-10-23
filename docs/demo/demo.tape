# SafeServe demonstration video. Re-record with:  vhs docs/demo/demo.tape
# (requires https://github.com/charmbracelet/vhs; run from the repo root after `make`)

Output docs/demo/safeserve-demo.mp4
Output docs/demo/safeserve-demo.gif

Set Shell "bash"
Set FontSize 15
Set Width 1400
Set Height 860
Set Padding 20
Set Theme "Catppuccin Mocha"
Set TypingSpeed 35ms
Set Framerate 24

Hide
Type "export PS1='\[\e[1;36m\]safeserve\[\e[0m\] $ ' && make -s >/dev/null 2>&1 && clear" Enter
Sleep 1s
Show

# --- 1. What it is ---------------------------------------------------------
Type "# SafeServe: a multi-threaded C web server that compares 4 deadlock strategies" Enter
Sleep 1500ms
Type "# naive (none) | lockorder (prevention) | detect (detection+recovery) | banker (avoidance)" Enter
Sleep 2s
Type "ls src/ && sed -n '/route \/upload/,/^$/p;/route \/refresh/,/^$/p' config/resources.conf" Enter
Sleep 5s
Type "# /upload takes CLOCK then DB, /refresh takes DB then CLOCK: opposite orders" Enter
Sleep 2s
Type "clear" Enter

# --- 2. Tests ----------------------------------------------------------------
Type "# 1) Test suite: textbook cases, brute-force property test, every mode, stress" Enter
Sleep 1s
Type "make test 2>&1 | grep -E 'PASS|stress\]|passed'" Enter
Sleep 30s
Type "clear" Enter

# --- 3. In-process demo ----------------------------------------------------
Type "# 2) One workload, four strategies (real resource manager, 2 threads)" Enter
Sleep 1s
Type "make -s demo | less -R" Enter
Sleep 9s
Type "/ mode = banker" Enter
Sleep 7s
Type "/ mode = lockorder" Enter
Sleep 6s
Type "/ mode = detect" Enter
Sleep 7s
Type "q"
Type "clear" Enter

# --- 4. Live server ----------------------------------------------------------
Type "# 3) The real HTTP server: fire 2 uploads + 4 refreshes at the same moment" Enter
Sleep 1s
Type "MODES='naive banker' ./tools/deadlock_demo.sh" Enter
Sleep 14s
Type "clear" Enter

Type "# Live metrics while the server runs" Enter
Type "./bin/safeserve --mode banker --port 8080 --quiet >/dev/null & sleep 0.5" Enter
Sleep 1s
Type "for i in 1 2 3 4; do curl -s -o /dev/null -X POST localhost:8080/refresh & curl -s -o /dev/null -X POST -d x localhost:8080/upload & done; wait" Enter
Sleep 2s
Type "curl -s localhost:8080/metrics | head -32" Enter
Sleep 7s
Type "kill -INT %1; wait 2>/dev/null; clear" Enter
Sleep 1s

# --- 5. Benchmark results --------------------------------------------------
Type "# 4) Benchmark: 5 strategies x 3 mixes x 3 load levels x 3 reps (results/bench.csv)" Enter
Sleep 1s
Type "python3 tools/plot.py --markdown | grep -E 'Mix|write \| 50'" Enter
Sleep 8s
Type "# naive freezes; banker trades some throughput for zero deadlocks; detect loses work to aborted victims" Enter
Sleep 4s
Type "# Full results and discussion: docs/REPORT.md" Enter
Sleep 3s
