#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Profile-guided optimization links of the Anka release programs (BUILDING_ANKA.md).
# Run from Git Bash started inside a Visual Studio 2022 x64 Developer shell (ninja.exe on PATH),
# after `cmake --build <build> --target yuzu-cmd yuzu -- -d keeprsp` with -DENABLE_LTO=ON.
# PGO_TARGET picks the program: yuzu-cmd (default, eden-cli) or yuzu (the menu program,
# eglantine). link.exe and pgomgr.exe are taken from the folder of the linker in the target's
# own link line (in Git Bash a bare `link.exe` is GNU coreutils' link). Nothing is compiled
# again: the same /GL objects are linked with other /LTCG options (and /CGTHREADS:8). The menu
# program's link line reads its inputs from CMakeFiles\yuzu.rsp, which ninja deletes after a
# successful link unless it ran with `-d keeprsp`; its content goes into the new response file
# (link.exe reads no nested response file). PGO_LINK_EXTRA adds link options, for example
# PGO_LINK_EXTRA='/PDBALTPATH:%_PDB%'.
#
#   pgo_link.sh <build dir> instrument <name>
#       bin/<name>.exe linked with /FASTGENPROFILE:PGD=bin\<name>.pgd. Play with it (pgort140.dll
#       from the MSVC tools next to it); it writes <name>!N.pgc files next to the .pgd, or into
#       the folder in VCPROFILE_PATH.
#   pgo_link.sh <build dir> optimize <name> <instrumented .pgd> <folder with .pgc files>
#       copies the .pgd to bin/<name>.pgd, merges every .pgc into it (pgomgr /merge), and links
#       bin/<name>.exe with /USEPROFILE:PGD=bin\<name>.pgd. The menu program takes eden-cli's
#       profile: the emulator code they share is the same, its own Qt code gets none.
set -euo pipefail
export MSYS2_ARG_CONV_EXCL='*'   # keep /options away from Git Bash's path conversion

[ $# -ge 3 ] || { sed -n '5,25p' "$0"; exit 2; }
B=$1 MODE=$2 NAME=$3
[[ "$NAME" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "error: name"; exit 2; }
TARGET=${PGO_TARGET:-yuzu-cmd}
case "$TARGET" in
  yuzu-cmd) OUT=eden-cli ;;
  yuzu) OUT=eglantine ;;
  *) echo "error: PGO_TARGET is yuzu-cmd or yuzu"; exit 2 ;;
esac

LINK_LINE=$(ninja -C "$B" -t commands "$TARGET" | tail -1)
case "$LINK_LINE" in *"/out:bin\\$OUT.exe"*'/LTCG '*) ;; *) echo "error: unexpected link line"; exit 1 ;; esac
MSVC_BIN=${LINK_LINE%%link.exe *}   # "... -- C:\...\bin\Hostx64\x64\"
MSVC_BIN=${MSVC_BIN##* -- }
MSVC_BIN=$(cygpath -u "${MSVC_BIN%'\'}")
[ -x "$MSVC_BIN/link.exe" ] && [ -x "$MSVC_BIN/pgomgr.exe" ] || { echo "error: no MSVC link.exe in $MSVC_BIN"; exit 1; }

link_with() {   # $1 = what replaces "/LTCG " in the target's own link line
  local cmd=$LINK_LINE rsp content
  cmd=${cmd#*link.exe }
  # What follows the link goes ("cd ." back, or the menu program's windeployqt step); the space
  # keeps a last "/LTCG" replaceable.
  cmd="${cmd%% && *} "
  cmd=${cmd//"/out:bin\\$OUT.exe"/"/out:bin\\$NAME.exe"}
  cmd=$(printf '%s' "$cmd" | sed -E "s#/implib:[^ ]+#/implib:bin\\\\$NAME.lib#; s#/pdb:[^ ]+#/pdb:bin\\\\$NAME.pdb#")
  cmd=${cmd//'/LTCG '/"$1 /CGTHREADS:8 ${PGO_LINK_EXTRA:-} "}
  case " $cmd " in *' /C '*|*' /D '*) echo "error: a cmd.exe option is left in the link options"; exit 1 ;; esac
  rsp=$(printf '%s' "$cmd" | grep -o '@CMakeFiles\\[A-Za-z0-9_.-]*\.rsp' | head -1 || true)
  if [ -n "$rsp" ]; then
    [ -f "$B/${rsp#@}" ] || { echo "error: $B/${rsp#@} is gone: link $OUT.exe again with ninja -d keeprsp"; exit 1; }
    content=$(tr '\r\n' '  ' < "$B/${rsp#@}")
    cmd=${cmd//"$rsp"/"$content"}
  fi
  printf '%s\n' "$cmd" > "$B/$NAME.rsp"
  (cd "$B" && "$MSVC_BIN/link.exe" "@$NAME.rsp")
}

case "$MODE" in
  instrument)
    link_with "/LTCG /FASTGENPROFILE:PGD=bin\\$NAME.pgd"
    cp "$MSVC_BIN/pgort140.dll" "$B/bin/"
    ;;
  optimize)
    [ $# -ge 5 ] || { echo "error: optimize needs <instrumented .pgd> <pgc folder>"; exit 2; }
    cp "$4" "$B/bin/$NAME.pgd"
    shopt -s nullglob
    pgcs=("$5"/*.pgc)
    [ ${#pgcs[@]} -gt 0 ] || { echo "error: no .pgc in $5"; exit 1; }
    for f in "${pgcs[@]}"; do "$MSVC_BIN/pgomgr.exe" /merge "$f" "$B/bin/$NAME.pgd"; done
    link_with "/LTCG /USEPROFILE:PGD=bin\\$NAME.pgd"
    ;;
  *) echo "error: mode is instrument or optimize"; exit 2 ;;
esac
echo "done: $B/bin/$NAME.exe"
