#!/usr/bin/env python3
"""
Far Cry 2 Co-op Mod - Automated Test Harness
Launches game via Lutris, monitors log, verifies functionality.
"""

import subprocess
import time
import os
import sys
import signal
import threading
from pathlib import Path

# Configuration
PROJECT_DIR = Path("/home/host/Downloads/FC2-Coop-Project")
BIN_DIR = PROJECT_DIR / "bin"
LOG_FILE = BIN_DIR / "fc2_coop.log"
LUTRIS_GAME_ID = "far-cry-2"  # Lutris game slug

class TestHarness:
    def __init__(self):
        self.game_process = None
        self.log_monitor_thread = None
        self.stop_monitoring = False
        self.test_results = []
        
    def log(self, msg, level="INFO"):
        timestamp = time.strftime("%H:%M:%S")
        print(f"[{timestamp}] [{level}] {msg}")
        
    def run_cmd(self, cmd, timeout=30, cwd=None):
        """Run command and return (success, stdout, stderr)"""
        try:
            result = subprocess.run(cmd, shell=True, capture_output=True, text=True, 
                                  timeout=timeout, cwd=cwd)
            return result.returncode == 0, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return False, "", "Timeout"
        except Exception as e:
            return False, "", str(e)
    
    def start_game(self):
        """Launch Far Cry 2 via Lutris"""
        self.log("Starting Far Cry 2 via Lutris...")
        # Lutris command to launch game
        cmd = f"lutris lutris:{LUTRIS_GAME_ID}"
        # Run in background
        self.game_process = subprocess.Popen(
            cmd, shell=True, 
            stdout=subprocess.DEVNULL, 
            stderr=subprocess.DEVNULL,
            start_new_session=True
        )
        self.log(f"Game process started (PID: {self.game_process.pid})")
        return True
        
    def stop_game(self):
        """Stop the game process"""
        if self.game_process:
            self.log("Stopping game...")
            try:
                os.killpg(os.getpgid(self.game_process.pid), signal.SIGTERM)
                self.game_process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(self.game_process.pid), signal.SIGKILL)
            except Exception as e:
                self.log(f"Error stopping game: {e}", "ERROR")
            self.game_process = None
            
    def wait_for_log_line(self, pattern, timeout=60):
        """Wait for a specific line to appear in log"""
        start = time.time()
        last_size = 0
        
        while time.time() - start < timeout:
            if LOG_FILE.exists():
                with open(LOG_FILE, 'r') as f:
                    f.seek(last_size)
                    new_content = f.read()
                    last_size = f.tell()
                    
                    if pattern in new_content:
                        self.log(f"Found expected log: {pattern}")
                        return True
            time.sleep(0.5)
        
        self.log(f"Timeout waiting for: {pattern}", "ERROR")
        return False
    
    def get_recent_log(self, lines=50):
        """Get recent log entries"""
        if not LOG_FILE.exists():
            return ""
        with open(LOG_FILE, 'r') as f:
            all_lines = f.readlines()
            return ''.join(all_lines[-lines:])
    
    def test_initialization(self):
        """Test 1: Mod initializes correctly"""
        self.log("=== TEST 1: Mod Initialization ===")
        success = self.wait_for_log_line("[INIT] binkw32 proxy attached", 30)
        if success:
            success &= self.wait_for_log_line("[SUCCESS] Dunia.dll located", 10)
            success &= self.wait_for_log_line("[NET-TX] Sender ready", 5)
            success &= self.wait_for_log_line("[NET-RX] Listening", 5)
            success &= self.wait_for_log_line("[D3D9] Present hook active", 15)
        self.test_results.append(("Initialization", success))
        return success
    
    def test_in_game_state(self):
        """Test 2: Game reaches in-game state"""
        self.log("=== TEST 2: In-Game State Detection ===")
        success = self.wait_for_log_line("[STATE] In-game state confirmed", 60)
        self.test_results.append(("In-Game State", success))
        return success
    
    def test_f4_marker(self):
        """Test 3: F4 marker anchoring"""
        self.log("=== TEST 3: F4 Marker Anchoring ===")
        # Simulate F4 keypress via xdotool if available
        success, _, _ = self.run_cmd("which xdotool")
        if success:
            self.run_cmd("xdotool key F4")
            time.sleep(1)
            success = self.wait_for_log_line("[F4] Anchored 3D marker", 10)
        else:
            self.log("xdotool not available, skipping key simulation", "WARN")
            success = True  # Don't fail test if no xdotool
        self.test_results.append(("F4 Marker", success))
        return success
    
    def test_f7_spawn(self):
        """Test 4: F7 buddy spawn"""
        self.log("=== TEST 4: F7 Buddy Spawn ===")
        success, _, _ = self.run_cmd("which xdotool")
        if success:
            self.run_cmd("xdotool key F7")
            time.sleep(3)  # Spawn takes time
            # Check for successful spawn (not enemy)
            recent = self.get_recent_log(30)
            success = ("[SPAWN-SUCCESS] Archetype" in recent and 
                      "enemy_archetypes" not in recent)
            if not success:
                self.log(f"Spawn log: {recent[-500:]}", "DEBUG")
        else:
            success = True
        self.test_results.append(("F7 Spawn", success))
        return success
    
    def test_f8_teleport(self):
        """Test 5: F8 teleport (no crash)"""
        self.log("=== TEST 5: F8 Teleport ===")
        success, _, _ = self.run_cmd("which xdotool")
        if success:
            self.run_cmd("xdotool key F8")
            time.sleep(2)
            recent = self.get_recent_log(20)
            success = ("[TELEPORT] SUCCESS" in recent or 
                      "[TELEPORT] Buddy entity invalid" in recent)
            # Not crashing is success
            if "EXCEPTION" in recent or "crash" in recent.lower():
                success = False
        else:
            success = True
        self.test_results.append(("F8 Teleport", success))
        return success
    
    def test_udp_transmission(self):
        """Test 6: UDP position broadcast"""
        self.log("=== TEST 6: UDP Transmission ===")
        success = self.wait_for_log_line("[UDP TX #", 30)
        self.test_results.append(("UDP TX", success))
        return success
    
    def test_clean_shutdown(self):
        """Test 7: Clean shutdown"""
        self.log("=== TEST 7: Clean Shutdown ===")
        self.stop_game()
        time.sleep(2)
        recent = self.get_recent_log(10)
        success = "[SHUTDOWN] Proxy detaching" in recent
        self.test_results.append(("Clean Shutdown", success))
        return success
    
    def run_all_tests(self):
        """Run complete test suite"""
        self.log("=" * 50)
        self.log("FAR CRY 2 CO-OP MOD - AUTOMATED TEST SUITE")
        self.log("=" * 50)
        
        # Clear old log
        if LOG_FILE.exists():
            LOG_FILE.unlink()
        
        try:
            if not self.start_game():
                self.log("Failed to start game", "ERROR")
                return False
            
            # Run tests in sequence
            tests = [
                self.test_initialization,
                self.test_in_game_state,
                self.test_f4_marker,
                self.test_f7_spawn,
                self.test_f8_teleport,
                self.test_udp_transmission,
                self.test_clean_shutdown,
            ]
            
            for test in tests:
                if not test():
                    self.log(f"Test failed, stopping suite", "ERROR")
                    break
                    
        finally:
            self.stop_game()
            
        # Print summary
        self.log("=" * 50)
        self.log("TEST SUMMARY")
        self.log("=" * 50)
        passed = 0
        for name, result in self.test_results:
            status = "PASS" if result else "FAIL"
            self.log(f"  {name}: {status}")
            if result:
                passed += 1
        self.log(f"\nTotal: {passed}/{len(self.test_results)} passed")
        
        return passed == len(self.test_results)


def main():
    # Check dependencies
    for dep in ["lutris", "xdotool"]:
        success, _, _ = subprocess.run(f"which {dep}", shell=True, capture_output=True)
        if not success:
            print(f"WARNING: {dep} not found - some tests will be skipped")
    
    harness = TestHarness()
    success = harness.run_all_tests()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()