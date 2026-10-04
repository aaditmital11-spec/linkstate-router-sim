# linkstate-router-sim

A small link-state routing simulator written in C11. Several copies of one
program run as separate Linux processes, each acting as a router. They talk to
each other over UDP on localhost, discover the whole network by flooding link
state advertisements, run Dijkstra's algorithm, and print their routing tables.

## Overview

This project is OSPF-inspired. It is **not** an OSPF implementation, and it is
not compatible with OSPF in any way.

What it borrows from OSPF is the shape of the protocol:

* periodic HELLO messages to discover neighbors and detect when they die,
* link state advertisements (LSAs) that each router floods to describe only its
  own directly attached links,
* sequence numbers to tell new information from old copies still circulating,
* a two-way check before a link is believed,
* aging, so information from a router that has stopped talking is discarded,
* shortest path first (Dijkstra) run locally over the resulting map.

There is also a minimal data plane: `DATA` packets are forwarded hop by hop
using the computed tables, with a TTL to bound looping. See "Forwarding" below.

What it leaves out is almost everything else: there are no areas, no router
types, no designated routers, no LSA types beyond the single one here, no
authentication, no IP prefixes, and no real packet formats. Messages are plain
ASCII text rather than OSPF's binary encoding, specifically so they are
readable in Wireshark while learning.

The design constraint that makes the simulation meaningful is this: a router
may read `topology.txt` only to learn the links it is itself attached to.
Everything it knows beyond its own neighbors has to arrive over the network in
an LSA. `topology.c` enforces that by discarding every line of the file that
does not mention the calling router's ID.

## Architecture

### The test network

Costs are shown on each link. Router 2 is the hub.

```
                 cost 5
        +-----------------------+
        |                       |
     +--+--+   cost 1    +------+   cost 1   +-----+
     | R1  +-------------+  R2  +------------+ R4  |
     +-----+             +---+--+            +-----+
                             |
                             | cost 1
                             |
                         +---+--+
                         |  R3  |
                         +------+
                             |
                             +----------- also linked to R1 at cost 5
```

Written out as the file contents:

```
1 2 1
1 3 5
2 3 1
2 4 1
```

Router N binds UDP `127.0.0.1:5000+N`, so R1 is on port 5001, R2 on 5002, and
so on. To reach neighbor N, a router simply sends to `127.0.0.1:5000+N`.

### Source layout

| File | Responsibility |
| --- | --- |
| `src/router.c` | `main()`, argument parsing, UDP socket, `poll()` event loop, timers, message formatting and parsing, neighbor liveness, LSA origination and flooding, data packet forwarding |
| `src/topology.c` | parse the topology file, return only this router's own links |
| `src/lsdb.c` | store one LSA per origin, sequence numbers, aging, two-way link check, the flooding decision |
| `src/spf.c` | Dijkstra, routing table construction, table printing, next-hop lookup |

### The per-router event loop

Everything happens in one thread. There are no threads, no blocking reads, and
no work done inside signal handlers.

```
                        +---------------------------+
                        |  load own links from file |
                        |  bind UDP 127.0.0.1:500N  |
                        |  originate first LSA      |
                        +-------------+-------------+
                                      |
                                      v
        +-----------------------------------------------------------+
        |  compute timeout = (time until soonest deadline)          |
        |     next HELLO                                            |
        |     next LSA refresh                                      |
        |     soonest neighbor dead-interval expiry                 |
        |     soonest stored LSA max-age expiry                     |
        |     --run-for deadline, if given                          |
        +-----------------------------+-----------------------------+
                                      |
                                      v
                        +---------------------------+
                        |  poll(socket, timeout)    |
                        +-------------+-------------+
                                      |
                 +--------------------+--------------------+
                 |                                         |
          packet readable                            timeout expired
                 |                                         |
                 v                                         v
     +-----------------------+                 +-------------------------+
     | recvfrom one datagram |                 |  nothing to read        |
     | HELLO: mark neighbor  |                 +-----------+-------------+
     |        up, refresh    |                             |
     | LSA:   ask the LSDB   |                             |
     |        if it is news; |                             |
     |        if so, flood   |                             |
     |        onward and     |                             |
     |        mark dirty     |                             |
     +-----------+-----------+                             |
                 |                                         |
                 +--------------------+--------------------+
                                      |
                                      v
        +-----------------------------------------------------------+
        |  service timers                                           |
        |    send HELLOs if due                                     |
        |    time out silent neighbors (log UP / DOWN transitions)   |
        |    re-originate our LSA on any change, or on refresh      |
        |    age out stale LSAs                                     |
        |    if the database changed, run Dijkstra and print the     |
        |      table only if it differs from the last one printed    |
        |    if the --run-for deadline passed, leave the loop        |
        +-----------------------------+-----------------------------+
                                      |
                        shutdown flag set, or deadline reached
                                      |
                                      v
                        +---------------------------+
                        |  print FINAL table, END   |
                        |  close socket, exit 0     |
                        +---------------------------+
```

The `poll()` timeout is computed as the time remaining until the soonest
pending deadline rather than being a fixed sleep. That way the process blocks
while idle and wakes exactly when there is work to do.

## How it works

### HELLO and neighbor liveness

Every 1000 ms a router sends `HELLO <sender_id>` to each of its configured
neighbors, including ones it currently believes are down, since that is how a
restarted neighbor gets rediscovered.

A neighbor is marked UP on its first HELLO and DOWN if no HELLO arrives for
3500 ms. The dead interval is three and a half times the HELLO interval, so a
single lost packet never causes a false alarm. Transitions are logged as
`[R1] neighbor 2 UP` and `[R1] neighbor 2 DOWN`.

A HELLO is only accepted if the sender ID inside the message matches the router
ID implied by the UDP source port, and if that router is a configured neighbor.
A stray packet therefore cannot invent an adjacency.

### LSA flooding and sequence numbers

An LSA is one router's statement about its own working links:

```
LSA <origin_id> <seq> <count> <nbr>:<cost> <nbr>:<cost> ...
```

For example, `LSA 2 7 3 1:1 3:1 4:1` means "router 2, revision 7, has three
links: to 1 at cost 1, to 3 at cost 1, and to 4 at cost 1".

A router originates a fresh LSA with an incremented sequence number in two
situations:

1. every 5000 ms as a routine refresh, which keeps its entry alive in everybody
   else's database, and
2. immediately whenever one of its own neighbors goes up or down, which is what
   makes the network reconverge in seconds instead of waiting for a timer.

Only neighbors that are currently UP are listed. Advertising a smaller set of
links is the entire mechanism by which a failure propagates.

On receiving an LSA, the database applies one rule. If there is no stored LSA
from that origin, or the arriving sequence number is greater than the stored
one, the LSA is stored with a receive timestamp, the arrival is logged, and the
original bytes are forwarded unchanged to every UP neighbor except the one it
came from. Otherwise it is ignored silently.

That silent ignore is what stops flooding from looping forever. The test
network contains a cycle (1 to 2 to 3 and back to 1), so a flooded LSA reaches
some routers twice. The first copy is news and gets forwarded. The second copy
carries a sequence number that is no longer greater than the stored one, so it
stops dead and the flood terminates.

Stored LSAs expire after 15000 ms without a refresh. A router's own LSA is
exempt, since it is the authority on its own links and refreshes them on a
timer. 15000 ms is three refresh periods, which leaves a wide margin so an
ordinary refresh is never mistaken for a dead router.

### The two-way check

When building the graph for Dijkstra, a link between A and B is included only
if A's stored LSA lists B **and** B's stored LSA lists A. The cost used is the
one A advertises.

Real OSPF does the same thing, and the reason is worth understanding. If router
A can still hear B but B cannot hear A, the link cannot carry traffic in both
directions and must not be routed over. The same check also handles stale
information cleanly: when a router dies, its neighbors stop advertising links
to it long before its own LSA ages out, so the two-way check fails immediately
and the dead router drops out of everyone's shortest path calculation right
away rather than 15 seconds later.

### Dijkstra

`spf_compute()` runs a plain O(N^2) array-based Dijkstra over router IDs 1 to
16, using `dist[]`, `prev[]` and `visited[]`. With at most 16 routers there is
no reason to use a priority queue, and the flat arrays are far easier to
inspect in a debugger and to explain out loud.

Ties are broken towards the lower router ID in two places: when choosing which
unvisited router to settle next, and when deciding which predecessor to record
for an equal-cost path. That makes the output identical on every run instead of
depending on array ordering.

For each destination, the next hop is the first router after us on the path,
recovered by walking the `prev[]` chain back from the destination to ourselves
and then reversing it.

Routes are recalculated whenever the database changes, but the table is printed
only when it differs from the last table printed. The logs therefore read as a
list of convergence events rather than one table per received packet.

### Forwarding

Everything above is the control plane: HELLOs, LSAs and Dijkstra exist to work
out where things are. Forwarding is the data plane, and its job is just to move
a packet one hop closer.

```
DATA <src> <dst> <ttl> <payload>
DATA 1 3 8 hello world
```

The payload is everything after the fourth field, so it may contain spaces. It
is opaque: the router never interprets it, it only copies it along.

On receiving a `DATA` packet a router does one of four things:

1. if `dst` is its own ID, deliver it and log the payload,
2. else if the ttl is 1 or less, drop it, since forwarding would take the ttl
   to zero,
3. else if `spf_next_hop()` returns -1, drop it because there is no route,
4. otherwise decrement the ttl, rebuild the message, and send it to the next
   hop.

The whole decision rests on one lookup in the table the control plane already
computed, which is exactly the division of labour a real router makes: the
control plane is slow and occasional, the data plane is fast and per packet.

Two details are worth knowing. First, the ttl is what stops a packet
circulating forever. During the brief window while a failure is still
propagating, two routers can disagree about the next hop and bounce a packet
between each other, and nothing but the ttl ends that.

Second, `DATA` is the one message type whose source port is *not* checked.
HELLOs and LSAs must come from a real router port so that a stray packet cannot
invent an adjacency or inject topology. A data packet is traffic rather than a
protocol message, and it can legitimately be injected by anything; `send.sh`
uses netcat, which sends from an ephemeral port belonging to no router at all.
The `src` and `dst` fields inside the message are what identify it.

## Build and run

Requirements: Linux, `gcc`, `make`. Tested on Ubuntu 24.04 under WSL2.

```sh
make
```

That produces `./router`, built with:

```
-std=c11 -Wall -Wextra -g -O0 -D_POSIX_C_SOURCE=200809L
```

The build is expected to be completely free of warnings.
`_POSIX_C_SOURCE=200809L` is needed because strict C11 otherwise hides the
POSIX interfaces this program uses, including `clock_gettime`, `poll` and
`sigaction`.

Run one router by hand:

```sh
./router <id> [topology_file] [--run-for SECONDS] [-v]
```

| Option | Meaning |
| --- | --- |
| `<id>` | this router's ID, 1 to 16, required |
| `topology_file` | link list, defaults to `topology.txt` |
| `--run-for SECONDS` | exit cleanly after SECONDS, printing a final table first |
| `-v` | also log every HELLO sent and received, which is noisy and off by default |

On SIGINT (Ctrl+C) or SIGTERM the router also prints its final table and exits
cleanly. The handler only sets a `volatile sig_atomic_t` flag, because `printf`
takes a lock on the stream and is not safe to call from a signal handler; the
table is printed afterwards from the normal flow of the program.

To start the whole network at once:

```sh
./run.sh
```

That builds the project, creates `logs/`, starts routers 1 through 4 in the
background with their output going to `logs/r<id>.log`, prints their PIDs, and
then follows all the logs. Press Ctrl+C to stop: every router is sent SIGTERM
so it prints its final table, and the script waits for them all to finish.

To push a data packet through a running network:

```sh
./send.sh <from> <to> <message>
./send.sh 1 3 hello world
```

The packet is handed to the router it originates from, which forwards it onward
from there. The message may contain spaces, and the starting ttl is 8.

## Example output

Startup and convergence, from `logs/r1.log`:

```
[R1] starting with 2 configured neighbor(s): 2(cost 1) 3(cost 5)
[R1] listening on 127.0.0.1:5001
[R1] new LSA from R1 seq 1
[R1] neighbor 2 UP
[R1] new LSA from R1 seq 2
[R1] neighbor 3 UP
[R1] new LSA from R1 seq 3
[R1] new LSA from R3 seq 2
[R1] new LSA from R2 seq 4
[R1] new LSA from R4 seq 2
Router 1 routing table
Destination   Next Hop   Cost   Path
2             2          1      1 -> 2
3             2          2      1 -> 2 -> 3
4             2          2      1 -> 2 -> 4
```

Note that router 1 reaches router 3 at cost 2 through router 2, not over its
own direct link at cost 5, and that it learned about router 4 even though it
has no link to it.

The converged tables for all four routers:

```
Router 1 routing table          Router 2 routing table
Destination   Next Hop   Cost   Destination   Next Hop   Cost
2             2          1      1             1          1
3             2          2      3             3          1
4             2          2      4             4          1

Router 3 routing table          Router 4 routing table
Destination   Next Hop   Cost   Destination   Next Hop   Cost
1             2          2      1             2          2
2             2          1      2             2          1
4             2          2      3             2          2
```

After router 2 is killed with SIGKILL, so that it dies without warning anybody,
the survivors reconverge onto the only remaining link:

```
FINAL
Router 1 routing table
Destination   Next Hop   Cost   Path
2             -          inf    unreachable
3             3          5      1 -> 3
4             -          inf    unreachable
END
```

Router 1 now uses its expensive direct link to router 3, and router 4 is cut
off from the network entirely because its only link was to router 2.

A data packet crossing the network, with one line from each of three separate
processes:

```
[R1] forwarding DATA 1->3 via R2 (ttl 7)
[R2] forwarding DATA 1->3 via R3 (ttl 6)
[R3] delivered DATA from R1: "hello world" (ttl 6)
```

Each router made its own decision from its own table. None of them knew the
whole path. With router 2 dead, the first line instead reads
`[R1] forwarding DATA 1->3 via R3` and the packet arrives in a single hop,
because the data plane simply follows wherever the control plane put the route.

Packets that cannot be delivered say why:

```
[R1] dropped DATA 1->4: no route
[R1] dropped DATA 1->3: ttl expired
```

## Testing

```sh
make test
```

which runs:

```sh
python3 -m pytest -v tests/
```

The tests are end to end. Each one starts real router processes that talk over
real UDP sockets, lets them converge, and then parses the block between the
`FINAL` and `END` markers in each process's output. Nothing is mocked.

| Test | What it checks |
| --- | --- |
| `test_full_topology` | runs all four routers for 8 seconds and asserts every expected next hop and cost in all four tables |
| `test_dijkstra_beats_direct_link` | router 1 reaches router 3 through router 2 at cost 2, not over its direct link at cost 5 |
| `test_router_failure` | runs all four for 30 seconds, kills router 2 with SIGKILL after 8 seconds, and asserts that routers 1 and 3 reconverge onto the cost 5 link and report router 4 as unreachable |
| `test_data_forwarding` | injects a packet at router 1 addressed to router 3 and asserts each hop logged its own forwarding decision, ending in delivery with the payload intact |
| `test_forwarding_after_failure` | kills router 2, waits for reconvergence, and asserts router 1 now forwards to router 3 over its direct link |
| `test_unreachable_dropped` | kills router 2, then asserts a packet for the cut-off router 4 is dropped at router 1 with `no route` |

SIGKILL is used in the failure test on purpose. It cannot be caught, so router
2 dies silently and the other routers have to work out that it is gone purely
from the absence of HELLOs. A fixture kills any leftover `router` processes
before and after each test, because a stale process holding a UDP port would
make `bind()` fail and the test would silently measure the wrong processes.

The whole suite takes roughly two minutes, most of which is the three failure
tests waiting for the dead interval and then for LSA aging.

The data plane tests inject their packets with a plain Python UDP socket rather
than by shelling out to `send.sh`, so the suite does not depend on netcat being
installed. The packet is byte for byte the same either way.

## Packet capture

Messages are plain ASCII, so they are readable directly in a capture. To record
the traffic between all four routers:

```sh
sudo tcpdump -i lo -nn -A udp portrange 5001-5004 -w docs/capture.pcap
```

`-i lo` is the loopback interface, `-nn` turns off name and port resolution,
`-A` prints payloads as ASCII, and `-w` writes a pcap file that Wireshark can
open. Drop the `-w` to watch the HELLO and LSA messages scroll past live.

Filtering with `udp contains "LSA"` in Wireshark isolates the link-state
traffic from the much more numerous HELLOs. Expanding the `Data` field of any
packet shows the message as readable text, for example `LSA 4 3 1 2:1`, with no
dissector required.

![Wireshark showing flooded LSAs on the loopback interface, with the message readable as ASCII in the Data field](docs/wireshark.png)

## Debugging with gdb

The binary is built with `-g -O0`, so every variable is real and line numbers
match the source exactly.

First start the other routers, otherwise the one under the debugger has nobody
to talk to and its database will only ever contain its own LSA:

```sh
for id in 2 3 4; do ./router $id topology.txt --run-for 120 >/dev/null & done
```

Then debug router 1:

```sh
$ gdb ./router
(gdb) break spf_compute
(gdb) run 1
(gdb) print dist
```

Note what `print dist` actually shows here. The breakpoint lands on the opening
brace of `spf_compute`, which is *before* the loop that fills the arrays in, so
at this point `dist` is whatever happened to be on the stack:

```
$1 = {540287026, 1862273585, -9544, 32767, 5, 0, 0, 0, ...}
```

That is uninitialised memory, not a bug. Step past the initialisation with
`next` a few times and the array becomes meaningful. The two recipes below are
more useful because they stop at a moment you actually care about.

### Watch Dijkstra settle one router at a time

`pick_closest_unvisited` is called once per iteration of the main loop, and its
arguments are the live arrays. The condition waits until a route to router 2
has been discovered, so the network has had time to converge:

```sh
(gdb) break pick_closest_unvisited if dist[2] != 2147483647
(gdb) run 1
(gdb) print *dist@17
(gdb) print *visited@17
(gdb) continue
```

```
$1 = {2147483647, 0, 1, 2147483647 <repeats 14 times>}
$2 = {0, 1, 0 <repeats 15 times>}
```

`dist` and `visited` are pointers here rather than arrays, so `print *dist@17`
is needed to print 17 elements starting at the pointer. Reading the output:
slot 0 is unused, slot 1 is ourselves at distance 0 and already visited, and
slot 2 is router 2 at distance 1, discovered but not yet settled. 2147483647 is
`INT_MAX`, which is how "no path known yet" is represented. Each `continue`
settles one more router.

### Inspect a fully converged routing table

Destination 4 is the furthest thing from router 1, so a non-infinite cost to it
means everything has converged. `-1` is `SPF_INFINITE_COST`:

```sh
(gdb) break spf_print_table if t->routes[2].cost != -1
(gdb) run 1
(gdb) print t->routes[0]
(gdb) print t->routes[1]
(gdb) print t->routes[2]
```

```
$1 = {dest = 2, next_hop = 2, cost = 1, path = {1, 2, 0 <repeats 15 times>}, path_len = 2}
$2 = {dest = 3, next_hop = 2, cost = 2, path = {1, 2, 3, 0 <repeats 14 times>}, path_len = 3}
$3 = {dest = 4, next_hop = 2, cost = 2, path = {1, 2, 4, 0 <repeats 14 times>}, path_len = 3}
```

`print *t` prints the whole table in one go, and `print *db` shows the
link-state database the routes were computed from.

Useful companions: `next` to step over a line, `step` to step into a call,
`finish` to run to the end of the current function, and `watch dist[3]` to stop
the moment the distance to router 3 changes.

To debug a router that is already running, attach to it with `gdb -p <pid>`.

![gdb stopped on a fully converged routing table, showing each route's next hop, cost and reconstructed path](docs/gdb.png)

## Limitations

This is a teaching simulator, and the following are known and deliberate.

* Data packets carry an opaque text payload and are addressed by router ID.
  There are no IP headers, no checksums, no fragmentation, and no congestion
  control, so this is a demonstration of forwarding rather than a real one.
* Localhost only, with addressing hardwired to `127.0.0.1:5000+id`. There is no
  real interface handling and no IP prefixes, only router IDs.
* At most 16 routers, since arrays are indexed directly by router ID.
* Messages are unauthenticated plain text. Any process that can bind a port in
  the 5001 to 5016 range can inject LSAs. The sender checks for HELLO are a
  sanity measure, not security.
* Sequence numbers are a 32 bit counter with no wraparound handling. A router
  would have to run for several thousand years at the current refresh rate to
  reach the limit, so it is ignored rather than solved.
* The set of destinations printed is sticky. Once a router has heard of another
  router it keeps that ID in a known-routers set forever, so the destination can
  be reported as unreachable instead of silently disappearing from the table
  when its LSA ages out. A long-lived process therefore never forgets a router
  that has permanently left the network.
* LSAs are flooded with no retransmission or acknowledgement. UDP on loopback
  does not lose packets in practice, and the periodic refresh covers the gap,
  but a lossy link would converge more slowly than it should.
* A single LSA must fit in one 512 byte datagram. With 16 routers and the cost
  ceiling of 65535 this cannot be exceeded, but there is no fragmentation.
* Link costs are read from a file at startup and never change. There is no
  measurement of actual delay, bandwidth or loss.

## Future work

* **Raspberry Pi deployment.** Run one instance per Pi on a real switched
  network instead of several processes on one host, which would replace the
  `5000+id` port convention with real interface addresses and introduce genuine
  packet loss and delay.
* **Network namespaces.** Use `ip netns` to give each router its own network
  stack and virtual ethernet pairs on a single Linux machine. That would exercise
  real routing tables and let link failure be simulated by taking an interface
  down rather than by killing a process.
* **VLANs.** Separate the simulated links onto tagged VLANs so that several
  logical topologies can share one physical switch, which is closer to how
  this kind of protocol is tested on real equipment.
