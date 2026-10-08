import subprocess
import re
import os

def generate():
    # 1. Parse exports from original binkw32.dll
    orig_dll = os.path.join("..", "bin", "binkw32.dll")
    if not os.path.exists(orig_dll):
        orig_dll = os.path.join("bin", "binkw32.dll")

    out = subprocess.check_output(["objdump", "-p", orig_dll]).decode("utf-8", errors="ignore")
    exports = [m.group(1) for m in re.finditer(r'\[\s*\d+\]\s*\+base\[\s*\d+\]\s+[0-9a-fA-F]{4}\s+(\S+)', out)]
    print(f"Found {len(exports)} exported functions in binkw32.dll")

    # 2. Generate binkw32.def for export forwarding to binkw32_orig.dll
    with open("binkw32.def", "w", encoding="utf-8") as f:
        f.write("LIBRARY binkw32\n")
        f.write("EXPORTS\n")
        for sym in exports:
            f.write(f"    {sym}=binkw32_orig.{sym}\n")
    print("Generated binkw32.def (forwarding to binkw32_orig.dll)")

if __name__ == "__main__":
    generate()
