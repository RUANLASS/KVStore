#!/usr/bin/env python3
"""
Replication test for kvserver/kvreplica, in the same style as
test_kvstore.py: drive the actual binaries and assert on output, but over
raw sockets instead of stdin.

Covers replication under load, READONLY enforcement on the replica,
replica-disconnect isolation (the primary keeps serving), and a late-joining
replica receiving a full snapshot via SYNC. Assertions poll with a bounded
timeout rather than checking immediately, since replication is asynchronous
and eventually consistent, not instant.
"""

import socket
import subprocess
import sys
import time

PRIMARY_PORT = 16380
REPLICA_PORT = 16381
LATE_REPLICA_PORT = 16382


def req(port, line, timeout=2):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall((line + "\n").encode())
    data = s.recv(4096)
    s.close()
    return data.decode().strip()


def wait_for_port(port, timeout=3):
    """Retries connecting instead of a fixed sleep, since how long a freshly
    started binary takes to reach listen() varies under load."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"nothing listening on port {port} after {timeout}s")


def poll_until(port, line, expected, timeout=3):
    """Bounded-timeout polling: replication is async, so a fresh write may
    not be visible on a replica yet. This is the honest way to test an
    eventually-consistent system rather than asserting instant visibility."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = req(port, line)
        if last == expected:
            return last
        time.sleep(0.05)
    return last


def check(label, actual, expected):
    status = "OK" if actual == expected else "FAIL"
    print(f"[{status}] {label}: got {actual!r}, expected {expected!r}")
    return actual == expected


def main():
    failures = 0

    primary = subprocess.Popen(["./kvserver", str(PRIMARY_PORT)])
    wait_for_port(PRIMARY_PORT)
    replica = subprocess.Popen(["./kvreplica", "127.0.0.1", str(PRIMARY_PORT), str(REPLICA_PORT)])
    wait_for_port(REPLICA_PORT)

    try:
        # Replication under load: several writes on the primary must show up
        # on the replica within the poll timeout.
        for i in range(20):
            req(PRIMARY_PORT, f"SET key{i} value{i}")

        for i in range(20):
            actual = poll_until(REPLICA_PORT, f"GET key{i}", f"VALUE value{i}")
            if not check(f"replicated key{i}", actual, f"VALUE value{i}"):
                failures += 1

        req(PRIMARY_PORT, "DEL key0")
        actual = poll_until(REPLICA_PORT, "GET key0", "NIL")
        if not check("replicated DEL key0", actual, "NIL"):
            failures += 1

        # READONLY enforcement on the replica.
        actual = req(REPLICA_PORT, "SET key0 nope")
        if not check("replica rejects writes", actual, "READONLY"):
            failures += 1

        # Replica disconnect must not affect the primary: kill the replica,
        # confirm the primary still serves clients normally.
        replica.terminate()
        replica.wait(timeout=2)
        time.sleep(0.3)

        actual = req(PRIMARY_PORT, "SET after_disconnect 1")
        if not check("primary still writable after replica dies", actual, "OK"):
            failures += 1
        actual = req(PRIMARY_PORT, "GET after_disconnect")
        if not check("primary still readable after replica dies", actual, "VALUE 1"):
            failures += 1

        # A late-joining replica must get a full snapshot: start a second
        # replica now, after the primary already has data, and confirm SYNC
        # delivers pre-existing keys, not just future writes.
        late_replica = subprocess.Popen(
            ["./kvreplica", "127.0.0.1", str(PRIMARY_PORT), str(LATE_REPLICA_PORT)]
        )
        wait_for_port(LATE_REPLICA_PORT)
        try:
            actual = poll_until(LATE_REPLICA_PORT, "GET key5", "VALUE value5")
            if not check("late replica gets pre-existing key via SYNC", actual, "VALUE value5"):
                failures += 1
            actual = poll_until(LATE_REPLICA_PORT, "GET after_disconnect", "VALUE 1")
            if not check("late replica gets data written after old replica died", actual, "VALUE 1"):
                failures += 1
        finally:
            late_replica.terminate()
            late_replica.wait(timeout=2)

    finally:
        primary.terminate()
        try:
            primary.wait(timeout=2)
        except subprocess.TimeoutExpired:
            primary.kill()
        if replica.poll() is None:
            replica.terminate()

    if failures:
        print(f"\n{failures} check(s) failed")
        sys.exit(1)
    print("\nAll replication checks passed")


if __name__ == "__main__":
    main()
