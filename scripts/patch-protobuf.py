#!/usr/bin/env python3
"""Patch protobuf 2.5.0 for Apple Silicon.

protobuf 2.5.0 predates arm64, so src/google/protobuf/stubs/platform_macros.h cannot
identify it. Inserting an __aarch64__ branch before the MIPS branch -- the last one in the
chain -- lets every other source compile unchanged.

The patch is idempotent: running it twice changes nothing.

Usage: patch-protobuf.py <path/to/platform_macros.h>
"""

import pathlib
import sys

MARKER = "#elif defined(__MIPSEL__)"
INSERT = (
    "#elif defined(__aarch64__) || defined(__arm64__)\n"
    "#define GOOGLE_PROTOBUF_ARCH_ARM 1\n"
    "#define GOOGLE_PROTOBUF_ARCH_64_BIT 1\n"
    "#elif defined(__MIPSEL__)"
)

path = pathlib.Path(sys.argv[1])
source = path.read_text()

if "__aarch64__" in source:
    print("== already patched: %s" % path)
    sys.exit(0)

path.write_text(source.replace(MARKER, INSERT, 1))
print("== patched %s for arm64" % path)
