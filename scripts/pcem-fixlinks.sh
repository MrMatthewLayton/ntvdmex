#!/usr/bin/env bash
# pcem-fixlinks.sh -- make the PCem 17 macOS bundle (pcem/PCem.app, gitignored) loadable.
#
# As shipped in the dmg the bundle does not start on a current macOS, and it fails
# BEFORE main(), so nothing is printed and PCem "idles at 0%" from outside:
#   1. its bundled wx/SDL dylibs still carry the Homebrew install names they were
#      built with (/usr/local/Cellar/wxmac/3.0.5.1_1/..., /usr/local/opt/jpeg/...),
#      so dyld aborts with "Library not loaded" -- the files ARE in the bundle, under
#      slightly different names;
#   2. it bundles a 2019 libSystem.B.dylib / libc++.1.dylib and points PCem at them
#      with @executable_path, and that libSystem's initializer segfaults on macOS 26.
# DYLD_LIBRARY_PATH cannot fix (1): every launcher on the way (zsh, perl, python)
# is a platform binary and dyld strips DYLD_* from a restricted process's environment.
# So rewrite the install names in place with install_name_tool. Idempotent.
#
# The image libraries wx wants (libpng16, libjpeg.9, libtiff.5) come from Homebrew;
# libjpeg.9 is mapped to jpeg-turbo's libjpeg.8 and libtiff.5 to the installed
# libtiff.6 -- fine for PCem, which never decodes an image through wx.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP="$ROOT/pcem/PCem.app/Contents/MacOS"
[ -x "$APP/PCem" ] || { echo "no PCem at $APP" >&2; exit 2; }
for l in /usr/local/opt/libpng/lib/libpng16.16.dylib \
         /usr/local/opt/jpeg-turbo/lib/libjpeg.8.dylib \
         /usr/local/opt/libtiff/lib/libtiff.6.dylib; do
  [ -f "$l" ] || { echo "missing $l (brew install libpng jpeg-turbo libtiff)" >&2; exit 2; }
done
cd "$APP"
changed=0
for f in PCem *.dylib; do
  otool -L "$f" 2>/dev/null | awk 'NR>1{print $1}' | while read -r dep; do
    leaf=$(basename "$dep")
    new=""
    case "$dep" in
      /usr/local/Cellar/wxmac/*/libwx_*-3.0.0.5.0.dylib) new="@loader_path/${leaf/-3.0.0.5.0.dylib/-3.0.dylib}" ;;
      /usr/local/opt/sdl2/lib/libSDL2-2.0.0.dylib)      new="@loader_path/libSDL2-2.0.0.dylib" ;;
      /usr/local/opt/libpng/lib/libpng16.16.dylib)      new="$dep" ;;
      /usr/local/opt/jpeg/lib/libjpeg.9.dylib)          new="/usr/local/opt/jpeg-turbo/lib/libjpeg.8.dylib" ;;
      /usr/local/opt/libtiff/lib/libtiff.5.dylib)       new="/usr/local/opt/libtiff/lib/libtiff.6.dylib" ;;
      @executable_path/libSystem.B.dylib|@loader_path/libSystem.B.dylib) new="/usr/lib/libSystem.B.dylib" ;;
      @executable_path/libc++.1.dylib|@loader_path/libc++.1.dylib)       new="/usr/lib/libc++.1.dylib" ;;
    esac
    if [ -n "$new" ] && [ "$new" != "$dep" ]; then
      install_name_tool -change "$dep" "$new" "$f" 2>&1 | grep -v 'cache file' || true
      echo "$f: $dep -> $new"
    fi
  done
done
echo "--- unresolved absolute deps (should be none beyond /usr/lib, /System and the three brew libs):"
for f in PCem *.dylib; do
  otool -L "$f" 2>/dev/null | awk 'NR>1{print $1}' | grep -E '^/usr/local|^@executable_path' \
    | grep -v -E 'libpng16|jpeg-turbo|libtiff/lib/libtiff\.6' | sed "s|^|$f: |" || true
done
echo "ok"
