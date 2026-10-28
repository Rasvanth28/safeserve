-- Start Live Demo: builds SafeServe, starts the four strategy servers in the
-- background (tools/live_demo.py) and opens the dashboard. No Terminal needed;
-- stop it with "Stop demo" on the dashboard.
set appPath to POSIX path of (path to me)
set projectDir to do shell script "dirname " & quoted form of appPath
set logFile to "/tmp/safeserve_live_demo.log"
set dashboard to "http://localhost:8080/"

-- Already running? Just open the dashboard.
try
	do shell script "curl -sf -m 1 -o /dev/null http://127.0.0.1:8080/healthz"
	open location dashboard
	return
end try

try
	do shell script "cd " & quoted form of projectDir & " && make -s > " & logFile & " 2>&1"
on error
	display dialog "SafeServe failed to build. Details are in " & logFile buttons {"OK"} default button 1 with icon stop
	return
end try

do shell script "(cd " & quoted form of projectDir & " && exec python3 -u tools/live_demo.py) >> " & logFile & " 2>&1 < /dev/null &"

-- Wait for the servers, then open the dashboard
repeat 30 times
	try
		do shell script "curl -sf -m 1 -o /dev/null http://127.0.0.1:8080/healthz"
		open location dashboard
		return
	end try
	delay 0.3
end repeat
display dialog "The demo did not start. Details are in " & logFile buttons {"OK"} default button 1 with icon stop
