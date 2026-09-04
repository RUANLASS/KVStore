#!/usr/bin/env python3
"""Stress test for kvstore: SET/GET/DELETE thousands of keys and verify correctness."""

import subprocess
import sys

N = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
BINARY = "./kvstore"

keys = [f"key{i}" for i in range(N)]
values = [f"value{i}" for i in range(N)]

commands = []
expected = []

# Insert all keys.
for k, v in zip(keys, values):
    commands.append(f"SET {k} {v}")
    expected.append("OK")

# Verify every key comes back with the right value.
for k, v in zip(keys, values):
    commands.append(f"GET {k}")
    expected.append(f"Key:{k}, Value:{v}")

# Delete the first half.
half = N // 2
for k in keys[:half]:
    commands.append(f"DELETE {k}")
    expected.append("OK")

# Deleted keys should now be missing.
for k in keys[:half]:
    commands.append(f"GET {k}")
    expected.append(f"Key:{k} does not exist")

# Untouched keys should still be present.
for k, v in zip(keys[half:], values[half:]):
    commands.append(f"GET {k}")
    expected.append(f"Key:{k}, Value:{v}")

# Deleting an already-deleted (or never-existing) key should report (nil).
for k in keys[:min(100, half)]:
    commands.append(f"DELETE {k}")
    expected.append("(nil)")

commands.append("EXIT")

print(f"Running {len(commands) - 1} commands against {BINARY} (N={N})...")

proc = subprocess.run(
    [BINARY],
    input="\n".join(commands) + "\n",
    capture_output=True,
    text=True,
    timeout=120,
)

lines = proc.stdout.splitlines()
# First line is the banner; every subsequent line is "> <response>".
responses = [line[2:] if line.startswith("> ") else line for line in lines[1:]]

failures = []
for i, (cmd, exp, got) in enumerate(zip(commands, expected, responses)):
    if got != exp:
        failures.append((i, cmd, exp, got))

if len(responses) < len(expected):
    print(f"MISMATCHED OUTPUT COUNT: expected {len(expected)} responses, got only {len(responses)}")

if failures:
    print(f"FAILED: {len(failures)} mismatches out of {len(expected)} commands\n")
    for i, cmd, exp, got in failures[:20]:
        print(f"  [{i}] cmd={cmd!r} expected={exp!r} got={got!r}")
    if len(failures) > 20:
        print(f"  ... and {len(failures) - 20} more")
    sys.exit(1)
else:
    print(f"PASSED: all {len(expected)} commands ({N} keys) behaved correctly.")
