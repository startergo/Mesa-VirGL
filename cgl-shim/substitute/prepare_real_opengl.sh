#!/bin/bash
# Prepare a renamed copy of the real OpenGL.framework binary for re-export
# from the substitute framework. The substitute links against this copy
# with -reexport_library, so CGL symbols we don't implement resolve
# against Apple's real implementation.
set -euo pipefail

GUEST=sl@slqemu.local
OUT=/Users/macbookpro/Mesa-VirGL/cgl-shim/substitute

# PRRT_kwDOPZhcm86uEfbO: Apple's OpenGL binary is PROPRIETARY — this
# fetch must land only in a local, untracked path. cgl-shim/substitute/
# libOpenGL_real_x86_64 is .gitignored for this reason; a copy was
# committed to this branch by accident once — do not re-add it, and ask
# the PR owner to remove the tracked one (binary deletions cannot be
# pushed from review tooling).

# Extract x86_64 slice from the guest's real OpenGL.framework
echo "==> extracting x86_64 slice from guest's real OpenGL"
ssh "$GUEST" 'lipo -extract x86_64 /System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL -output /tmp/OpenGL_x86_64_real' 2>&1 | tail -1
scp "$GUEST:/tmp/OpenGL_x86_64_real" "$OUT/libOpenGL_real_x86_64" 2>&1 | tail -1
if [ -d "$OUT/.git" ] || git -C "$OUT" check-ignore -q "$OUT/libOpenGL_real_x86_64" 2>/dev/null; then
    :   # scratch OUT dirs are fine either way
else
    echo "WARNING: $OUT is inside a git repository and libOpenGL_real_x86_64"
    echo "         is Apple proprietary — make sure it is gitignored."
fi

# Change install_name so it doesn't conflict with the substitute's
echo "==> changing install_name to @rpath/libOpenGL_real.dylib"
install_name_tool -id @rpath/libOpenGL_real.dylib "$OUT/libOpenGL_real_x86_64"

# Verify
echo "==> verify"
otool -D "$OUT/libOpenGL_real_x86_64"
echo "CGL exports: $(nm -g "$OUT/libOpenGL_real_x86_64" 2>/dev/null | grep -c ' T _CGL')"
echo "gl exports: $(nm -g "$OUT/libOpenGL_real_x86_64" 2>/dev/null | grep -cE ' T _gl[A-Z]')"
