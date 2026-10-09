#!/usr/bin/env python3
import subprocess, sys, os, time
log_dir = '/home/host/Downloads/Far Cry 2/logs'
os.makedirs(log_dir, exist_ok=True)
exe_path = '/home/host/Downloads/Far Cry 2/bin/FC2ServerLauncher.exe'
print(f"Starting: {exe_path}", flush=True)
proc = subprocess.Popen([exe_path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
print("=" * 60, flush=True)
try:
    while True:
        line = proc.stdout.readline()
        if not line and proc.poll() is not None: break
        if line:
            print(line.strip(), end='', flush=True)
except Exception as e:
    print(f"Exception: {e}", file=sys.stderr)
finally:
    proc.terminate()
