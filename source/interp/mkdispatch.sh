#!/bin/bash
# mkdispatch.sh <out> - build the dispatcher (the one program the kernel hook starts). A2X_SHIP=1 selects the shipped flavour.
set -u
D=$(cd "$(dirname "$0")" && pwd); O=${1:?output path}
X=${ARM2X86_LLVM:-/opt/homebrew/Cellar/llvm/22.1.8/bin}
SDK=${HOST_SDK:-$(xcrun --show-sdk-path)}
MINF=${HOST_MINOS:+-mmacosx-version-min=$HOST_MINOS}
LDF=${HOST_LD_DIR:+-B$HOST_LD_DIR -fuse-ld=$HOST_LD_DIR/ld}
"$X/clang" -arch x86_64 -isysroot "$SDK" $MINF $LDF -O2 -w -include "$D/a2x_flavour.h" ${A2X_SHIP:+-DA2X_SHIP=1 -ffile-prefix-map="$D"=. -Wl,-x} -o "$O" "$D/a2xdispatch.c" || exit 2
echo "dispatch: $O"
