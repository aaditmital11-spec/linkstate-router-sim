"""
Integration tests for the link-state routing simulator.

These are deliberately end-to-end: each test starts real router processes that
talk to each other over real UDP sockets, lets them converge, and then checks
the routing tables they print on the way out. Nothing is mocked, so a passing
run means the whole system actually works, flooding and timers included.

Only the Python standard library is used, besides pytest itself.
"""

import socket
import subprocess
import time
from pathlib import Path

import pytest

# The repository root is the parent of the tests/ directory holding this file.
REPO_ROOT = Path(__file__).resolve().parent.parent
ROUTER_BIN = REPO_ROOT / "router"
TOPOLOGY = REPO_ROOT / "topology.txt"

# Every router in the test network.
ALL_ROUTERS = [1, 2, 3, 4]

# Router N listens on 127.0.0.1:BASE_PORT+N, the same convention send.sh uses.
BASE_PORT = 5000

# How long to let the network settle before we trust a table. Convergence
# needs one HELLO interval to bring links up plus a moment to flood the LSAs,
# so a few seconds is plenty; 8 leaves generous margin on a loaded machine.
CONVERGE_SECONDS = 8

# The failure test needs time for three separate things to happen in sequence:
# the dead interval to expire (3.5 s), the new LSAs to flood, and the dead
# router's LSA to age out of everybody's database (15 s).
FAILURE_RUN_SECONDS = 30
FAILURE_KILL_AT_SECONDS = 8

# How long to wait after killing a router before the survivors can be trusted
# to have reconverged and stopped routing through it.
RECONVERGE_SECONDS = 15

# Long enough to converge, inject a packet, and let the logs flush.
DATA_RUN_SECONDS = 14

# Converge, kill the hub, reconverge, inject, flush.
DATA_FAILURE_RUN_SECONDS = FAILURE_KILL_AT_SECONDS + RECONVERGE_SECONDS + 3

# Expected tables once all four routers are up, as {dest: (next_hop, cost)}.
#
# Worth reading alongside topology.txt (1-2 cost 1, 1-3 cost 5, 2-3 cost 1,
# 2-4 cost 1): router 2 is the hub, so almost everything routes through it.
EXPECTED_FULL_TOPOLOGY = {
    1: {2: (2, 1), 3: (2, 2), 4: (2, 2)},
    2: {1: (1, 1), 3: (3, 1), 4: (4, 1)},
    3: {1: (2, 2), 2: (2, 1), 4: (2, 2)},
    4: {1: (2, 2), 2: (2, 1), 3: (2, 2)},
}

# Expected rows after router 2 dies. With the hub gone, the only surviving link
# is 1-3 at cost 5, and router 4 is cut off from the network entirely.
UNREACHABLE = (None, None)
EXPECTED_AFTER_FAILURE = {
    1: {3: (3, 5), 4: UNREACHABLE},
    3: {1: (1, 5), 4: UNREACHABLE},
}


@pytest.fixture(scope="session", autouse=True)
def build_router():
    """Build the binary once before any test runs."""
    subprocess.run(["make"], cwd=REPO_ROOT, check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert ROUTER_BIN.is_file(), f"make did not produce {ROUTER_BIN}"


@pytest.fixture(autouse=True)
def no_leftover_routers():
    """
    Make sure no router from an earlier run is still holding a UDP port.

    A leftover process would make bind() fail with EADDRINUSE, and the test
    would then be measuring the old processes instead of the new ones. We clean
    up both before and after, so one crashed test cannot poison the next.
    """
    _kill_leftover_routers()
    yield
    _kill_leftover_routers()


def _kill_leftover_routers():
    # -x matches the executable name exactly, so this cannot hit anything else
    # that merely has "router" somewhere in its command line. A non-zero exit
    # just means there was nothing to kill.
    subprocess.run(["pkill", "-x", "router"], check=False)
    # Give the kernel a moment to release the ports before the next bind().
    time.sleep(0.5)


def start_routers(router_ids, run_for):
    """
    Start one process per router ID, each exiting by itself after run_for
    seconds. Returns {router_id: Popen}, with stdout captured as text.
    """
    procs = {}

    for rid in router_ids:
        procs[rid] = subprocess.Popen(
            [str(ROUTER_BIN), str(rid), str(TOPOLOGY), "--run-for", str(run_for)],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    return procs


def collect_output(procs, timeout):
    """
    Wait for every router to exit and return {router_id: stdout text}.

    communicate() is used rather than wait() because it drains the pipe while
    waiting; wait() on a process with a full pipe would deadlock.
    """
    outputs = {}

    for rid, proc in procs.items():
        stdout, _ = proc.communicate(timeout=timeout)
        outputs[rid] = stdout

    return outputs


def inject_data(at_router, src, dst, message, ttl=8):
    """
    Hand one DATA packet to a running router, which should then forward it.

    This is what send.sh does, in Python rather than through netcat so the
    tests do not depend on an external tool. Note that the packet arrives from
    an ephemeral port belonging to no router, which is exactly why the router
    must not apply its source-port check to DATA.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        packet = f"DATA {src} {dst} {ttl} {message}".encode()
        sock.sendto(packet, ("127.0.0.1", BASE_PORT + at_router))
    finally:
        sock.close()


def parse_final_table(output):
    """
    Pull the block between the FINAL and END markers out of a router's stdout
    and return it as {dest: (next_hop, cost)}.

    An unreachable destination is reported as (None, None), matching the
    "-  inf  unreachable" row the router prints.
    """
    lines = output.splitlines()

    if "FINAL" not in lines:
        raise AssertionError(f"no FINAL marker in output:\n{output}")
    start = lines.index("FINAL")

    if "END" not in lines[start:]:
        raise AssertionError(f"no END marker after FINAL:\n{output}")
    end = lines.index("END", start)

    table = {}

    for line in lines[start + 1:end]:
        # Skip the title line and the column headings.
        if not line.strip() or line.startswith(("Router ", "Destination")):
            continue

        # Columns are space separated: destination, next hop, cost, then the
        # path, which itself contains spaces and is not needed here.
        fields = line.split()
        dest = int(fields[0])

        if fields[2] == "inf":
            table[dest] = UNREACHABLE
        else:
            table[dest] = (int(fields[1]), int(fields[2]))

    return table


def parse_final_paths(output):
    """Same block as parse_final_table, but returns {dest: path string}."""
    lines = output.splitlines()
    start = lines.index("FINAL")
    end = lines.index("END", start)

    paths = {}

    for line in lines[start + 1:end]:
        if not line.strip() or line.startswith(("Router ", "Destination")):
            continue

        fields = line.split()
        dest = int(fields[0])
        paths[dest] = " ".join(fields[3:])

    return paths


def test_full_topology():
    """With all four routers up, every table must match the expected routes."""
    procs = start_routers(ALL_ROUTERS, CONVERGE_SECONDS)
    outputs = collect_output(procs, timeout=CONVERGE_SECONDS + 30)

    for rid in ALL_ROUTERS:
        assert procs[rid].returncode == 0, f"router {rid} exited non-zero"

        table = parse_final_table(outputs[rid])

        for dest, expected in EXPECTED_FULL_TOPOLOGY[rid].items():
            assert dest in table, (
                f"router {rid} has no route to {dest}\n{outputs[rid]}")
            assert table[dest] == expected, (
                f"router {rid} route to {dest}: "
                f"expected next hop/cost {expected}, got {table[dest]}\n"
                f"{outputs[rid]}")


def test_dijkstra_beats_direct_link():
    """
    Router 1 has a direct link to router 3 costing 5, and a two-hop path
    through router 2 costing 1 + 1 = 2. Shortest path first must prefer the
    cheaper two-hop path, which is the whole point of running Dijkstra instead
    of just using the directly attached links.
    """
    procs = start_routers(ALL_ROUTERS, CONVERGE_SECONDS)
    outputs = collect_output(procs, timeout=CONVERGE_SECONDS + 30)

    table = parse_final_table(outputs[1])
    paths = parse_final_paths(outputs[1])

    next_hop, cost = table[3]

    assert cost == 2, f"expected cost 2 through router 2, got {cost}"
    assert cost != 5, "router 1 fell back to its direct cost 5 link"
    assert next_hop == 2, f"expected next hop 2, got {next_hop}"
    assert paths[3] == "1 -> 2 -> 3", f"unexpected path {paths[3]!r}"


def test_router_failure():
    """
    Kill the hub router and check the survivors reconverge.

    SIGKILL is used on purpose: it cannot be caught, so router 2 dies without
    printing anything and without telling anybody. The other routers have to
    work out that it is gone from the absence of HELLOs alone, which is exactly
    the failure the dead interval and LSA aging exist to handle.
    """
    procs = start_routers(ALL_ROUTERS, FAILURE_RUN_SECONDS)

    time.sleep(FAILURE_KILL_AT_SECONDS)
    procs[2].kill()

    outputs = collect_output(procs, timeout=FAILURE_RUN_SECONDS + 30)

    for rid, expected_rows in EXPECTED_AFTER_FAILURE.items():
        assert procs[rid].returncode == 0, f"router {rid} exited non-zero"

        table = parse_final_table(outputs[rid])

        for dest, expected in expected_rows.items():
            assert dest in table, (
                f"router {rid} has no row for {dest} after the failure\n"
                f"{outputs[rid]}")
            assert table[dest] == expected, (
                f"router {rid} route to {dest} after the failure: "
                f"expected {expected}, got {table[dest]}\n{outputs[rid]}")


def test_data_forwarding():
    """
    A data packet injected at router 1 for router 3 must travel 1 to 2 to 3,
    with each router making its own independent forwarding decision from its
    own table. Nobody is told the path; each hop only knows its next hop.
    """
    procs = start_routers(ALL_ROUTERS, DATA_RUN_SECONDS)

    time.sleep(CONVERGE_SECONDS)
    inject_data(at_router=1, src=1, dst=3, message="hello world")

    outputs = collect_output(procs, timeout=DATA_RUN_SECONDS + 30)

    assert "forwarding DATA 1->3 via R2" in outputs[1], outputs[1]
    assert "forwarding DATA 1->3 via R3" in outputs[2], outputs[2]
    assert "delivered DATA from R1" in outputs[3], outputs[3]

    # The payload must survive the trip intact, spaces included.
    assert 'delivered DATA from R1: "hello world"' in outputs[3], outputs[3]


def test_forwarding_after_failure():
    """
    Kill the hub and the data plane must follow the control plane onto the new
    path. Router 1 originally forwards to router 3 through router 2; once
    router 2 is gone it has to use its direct cost 5 link instead.
    """
    procs = start_routers(ALL_ROUTERS, DATA_FAILURE_RUN_SECONDS)

    time.sleep(FAILURE_KILL_AT_SECONDS)
    procs[2].kill()

    time.sleep(RECONVERGE_SECONDS)
    inject_data(at_router=1, src=1, dst=3, message="after the failure")

    outputs = collect_output(procs, timeout=DATA_FAILURE_RUN_SECONDS + 30)

    assert "forwarding DATA 1->3 via R3" in outputs[1], outputs[1]

    # One hop now instead of two, so it should arrive directly.
    assert "delivered DATA from R1" in outputs[3], outputs[3]


def test_unreachable_dropped():
    """
    Router 4's only link was through router 2, so killing router 2 cuts it off
    completely. A packet for router 4 must be dropped at router 1 with a clear
    reason rather than being forwarded into a black hole.
    """
    procs = start_routers(ALL_ROUTERS, DATA_FAILURE_RUN_SECONDS)

    time.sleep(FAILURE_KILL_AT_SECONDS)
    procs[2].kill()

    time.sleep(RECONVERGE_SECONDS)
    inject_data(at_router=1, src=1, dst=4, message="nobody home")

    outputs = collect_output(procs, timeout=DATA_FAILURE_RUN_SECONDS + 30)

    assert "dropped DATA 1->4: no route" in outputs[1], outputs[1]
