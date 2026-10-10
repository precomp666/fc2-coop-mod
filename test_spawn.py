#!/usr/bin/env python3
"""
Quick spawn test - launches game, presses F7, checks for success
"""

import subprocess
import time
import os
import sys

LOG_FILE = "/home/host/Downloads/FC2-Coop-Project/bin/fc2_coop.log"
PROJECT_DIR = "/home/host/Downloads/FC2-Coop-Project"

def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}")

def run_cmd(cmd):
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30)
        return result.returncode == 0, result.stdout, result.stderr
    except Exception as e:
        return False, "", str(e)

def wait_for_log(pattern, timeout=60):
    start = time.time()
    last_size = 0
    while time.time() - start < timeout:
        if os.path.exists(LOG_FILE):
            with open(LOG_FILE, 'r') as f:
                f.seek(last_size)
                content = f.read()
                last_size = f.tell()
                if pattern in content:
                    return True
        time.sleep(0.5)
    return False

def main():
    log("Building mod...")
    os.chdir("/home/host/Downloads/FC2-Coop-Project/coop_dev")
    subprocess.run(["./build.sh"], check=True)
    
    log("Clearing old log...")
    if os.path.exists(LOG_FILE):
        os.remove(LOG_FILE)
    
    log("Launching game via Lutris...")
    subprocess.Popen(["lutris", "lutris:far-cry-2"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    
    log("Waiting for in-game state...")
    if not wait_for_log("In-game state confirmed", 120):
        log("Timeout waiting for in-game", "ERROR")
        return 1
    
    log("In-game! Waiting 2s for stability...")
    time.sleep(2)
    
    log("Pressing F7 to spawn buddy...")
    run_cmd("xdotool key F7")
    
    log("Waiting for spawn result (10s)...")
    time.sleep(10)
    
    # Check log
    if os.path.exists(LOG_FILE):
        with open(LOG_FILE, 'r') as f:
            content = f.read()
            
        # Look for spawn results
        if "SPAWN_RAW_RESULT" in content:
            log("Found SPAWN_RAW_RESULT in log")
            for line in content.split('\n'):
                if "SPAWN_" in line:
                    log(f"  {line.strip()}")
        
        if "SPAWN-SUCCESS" in content:
            log("SUCCESS: Buddy spawned!")
            return 0
        elif "SPAWN_FAILED" in content:
            log("FAILED: Spawn returned invalid ID")
            for line in content.split('\n'):
                if "SPAWN_FAILED" in line:
                    log(f"  {line.strip()}")
            return 1
        else:
            log("No clear spawn result found")
            # Show recent spawn logs
            for line in content.split('\n'):
                if "SPAWN" in line:
                    log(f"  {line.strip()}")
            return 1
    else:
        log("Log file not found")
        return 1

if __name__ == "__main__":
    sys.exit(main())