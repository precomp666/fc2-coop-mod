#!/usr/bin/env python3
import subprocess, sys, os, signal, time
log_dir = '/home/host/Downloads/Far Cry 2/logs'
os.makedirs(log_dir, exist_ok=True)
exe_path = '/home/host/Downloads/Far Cry 2/bin/FC2ServerLauncher.exe'
def sigterm_handler(signum, frame):
    print("\n[MONITOR] Shutting down gracefully...")
signal.signal(signal.SIGTERM, sigterm_handler)
signal.signal(signal.SIGINT, sigterm_handler)
print(f"[*] Monitoring {exe_path}", flush=True)
proc = subprocess.Popen([exe_path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
def monitor():
    try:
        while proc.poll() is None:
            line = proc.stdout.readline()
            if line:
                print(line.strip(), end='', flush=True)
                with open('/home/host/Downloads/Far Cry 2/logs/FC2ServerLauncher.log', 'a') as f:
                    f.write(line)
        return proc.returncode
    except Exception as e:
        print(f"Monitor error: {e}", file=sys.stderr)
    finally:
        proc.terminate()
        print(f"\n[*] Server stopped. Log size: {os.path.getsize('/home/host/Downloads/Far Cry 2/logs/FC2ServerLauncher.log')} bytes", flush=True)
if __name__ == '__main__':
    monitor()
