#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

echo "==> Generating binkw32.def..."
python3 generate_proxy.py

# Auto-detect llvm-mingw in ~/tools if present
for tool_dir in "$HOME/tools"/llvm-mingw*; do
    if [ -d "$tool_dir/bin" ]; then
        export PATH="$tool_dir/bin:$PATH"
        break
    fi
done

COMPILER="i686-w64-mingw32-clang++"
if ! command -v "$COMPILER" &> /dev/null; then
    COMPILER="i686-w64-mingw32-g++"
fi

if ! command -v "$COMPILER" &> /dev/null; then
    echo "ERROR: Cross-compiler '$COMPILER' not found!"
    exit 1
fi


echo "==> Compiling proxy binkw32.dll with UDP networking & D3D9 3D Marker Overlay..."
"$COMPILER" -shared -s -O2 main.cpp binkw32.def -o binkw32.dll -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup -lws2_32 -ld3d9


echo "==> Installing proxy DLL to ../bin/..."
if [ ! -f "../bin/binkw32_orig.dll" ]; then
    echo "Backing up original binkw32.dll -> ../bin/binkw32_orig.dll"
    cp "../bin/binkw32.dll" "../bin/binkw32_orig.dll"
fi

cp binkw32.dll "../bin/binkw32.dll"
echo "==> DONE! binkw32.dll installed into Far Cry 2 /bin/"
echo "Now start the game in Lutris. It will log to 'bin/fc2_coop.log'."
