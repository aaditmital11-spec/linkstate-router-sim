/*
 * router.c - one process, one router.
 *
 * Several copies of this program run at the same time on one Linux machine.
 * Each copy is told its router ID on the command line, binds a UDP socket on
 * 127.0.0.1 port 5000 + ID, and talks to its neighbors at their own
 * 5000 + ID ports. Everything happens in a single thread driven by one
 * poll() loop: there are no threads, no signals-as-control-flow, and no
 * blocking reads, so the whole program is easy to reason about and to step
 * through in gdb.
 *
 * Responsibilities of this file: command line parsing, the UDP socket, the
 * poll() event loop and its timers, formatting and parsing messages, neighbor
 * liveness, and originating and flooding our own LSA. Storing LSAs and
 * deciding what is worth flooding lives in lsdb.c; route calculation lives in
 * spf.c.
 *
 * _POSIX_C_SOURCE=200809L is supplied by the Makefile; strict C11 would
 * otherwise hide clock_gettime() and poll().
 */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "lsdb.h"
#include "spf.h"
#include "topology.h"

/* Router N listens on 127.0.0.1:(BASE_PORT + N). Because every router follows
 * the same rule, knowing a neighbor's ID is enough to address it. */
#define BASE_PORT 5000
#define LOCALHOST "127.0.0.1"

/* Protocol messages are short ASCII lines. 512 bytes is the agreed ceiling,
 * which is comfortably more than the largest LSA we can generate. */
#define MAX_MSG 512

/* How often we send a HELLO to each configured neighbor. */
#define HELLO_INTERVAL_MS 1000

/* How long we tolerate silence from a neighbor before declaring it down.
 * Three and a half HELLO intervals, so a single lost packet is harmless. */
#define DEAD_INTERVAL_MS 3500

/* How often we re-originate our own LSA even when nothing has changed. The
 * periodic refresh is what keeps our LSA alive in everybody else's database. */
#define LSA_REFRESH_MS 5000

/* How long we keep somebody else's LSA without hearing a newer copy. It must
 * be comfortably larger than LSA_REFRESH_MS so that an ordinary refresh is
 * never mistaken for a dead router; three refresh periods gives that margin. */
#define LSA_MAX_AGE_MS 15000

/* Largest time to live we accept on a data packet. Any sane value is far below
 * this; the cap exists only so a malformed field cannot get through. */
#define MAX_TTL 255

/*
 * What we know about one directly connected neighbor. The id and cost come
 * from the topology file and never change. The up flag and last_hello_ms are
 * liveness state maintained from received HELLOs.
 */
struct neighbor {
    int id;                   /* neighbor's router ID */
    int cost;                 /* cost of our link to it */
    int up;                   /* 1 once we have heard a HELLO and not timed out */
    long long last_hello_ms;  /* monotonic time of the most recent HELLO */
};

/*
 * All per-process state, passed around by pointer. Keeping it in one struct
 * instead of using globals makes it obvious what each function can touch.
 */
struct router {
    int id;                                     /* our own router ID */
    int sock;                                   /* bound UDP socket */
    int verbose;                                /* -v: also log every HELLO */
    struct neighbor neighbors[MAX_NEIGHBORS];   /* our directly attached links */
    int neighbor_count;

    struct lsdb lsdb;                           /* everything we know about the network */
    unsigned int own_seq;                       /* sequence number of our last LSA */

    /* Set when a neighbor goes up or down. Our own link state has changed, so
     * we must re-originate immediately rather than wait for the refresh timer;
     * that is what makes the network reconverge in seconds. */
    int neighbor_changed;

    /* Set when the database changed, meaning the routing table may be stale. */
    int routes_dirty;

    struct routing_table table;     /* most recently computed table */
    struct routing_table printed;   /* most recently printed table */
    int have_printed;               /* 0 until we have printed anything */
};

/* ------------------------------------------------------------------ */
/* Signal handling                                                    */
/* ------------------------------------------------------------------ */

/*
 * Set to 1 when SIGINT or SIGTERM arrives. The event loop notices on its next
 * pass and shuts down from normal code.
 *
 * volatile stops the compiler from caching the value in a register across the
 * loop, since nothing in the loop body visibly assigns it. sig_atomic_t is the
 * one type the C standard guarantees can be written by a handler and read by
 * the main flow without tearing.
 */
static volatile sig_atomic_t g_shutdown = 0;

/*
 * The only thing the handler does is set the flag.
 *
 * A handler may only call async-signal-safe functions, and printf() is not one
 * of them: it takes a lock on the stream, so a signal arriving while main code
 * is mid-printf would deadlock. Setting a flag and printing later from the
 * normal flow avoids the whole problem.
 */
static void on_shutdown_signal(int sig)
{
    (void)sig;          /* same action for both signals; the number is unused */
    g_shutdown = 1;
}

/*
 * Install the handler for one signal. Returns 0 on success, -1 on failure.
 *
 * SA_RESTART is deliberately NOT set. We want the signal to interrupt poll()
 * and make it fail with EINTR, because that is what wakes the loop up
 * immediately instead of leaving it blocked until its next timer.
 */
static int install_signal_handler(int sig)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_shutdown_signal;
    sa.sa_flags = 0;

    /* Block nothing extra while the handler runs: it only sets a flag. */
    if (sigemptyset(&sa.sa_mask) != 0) {
        perror("sigemptyset");
        return -1;
    }

    if (sigaction(sig, &sa, NULL) != 0) {
        perror("sigaction");
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Time                                                               */
/* ------------------------------------------------------------------ */

/*
 * Milliseconds from an arbitrary fixed point, using CLOCK_MONOTONIC.
 *
 * CLOCK_MONOTONIC never jumps backwards and is unaffected by NTP steps or the
 * user changing the system clock, which is exactly what we want for measuring
 * intervals. CLOCK_REALTIME would let a clock adjustment make a neighbor look
 * dead or immortal.
 */
static long long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        /* There is no sensible way to continue without a clock. */
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ */
/* Logging helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Copy src into dst keeping only printable ASCII, replacing anything else
 * with '.', and always NUL-terminating. Used before logging a message we have
 * decided is malformed: the bytes came off the network, so they could contain
 * control characters that would corrupt the log or the terminal.
 */
static void sanitize(const char *src, size_t len, char *dst, size_t dstsz)
{
    size_t out = 0;

    if (dstsz == 0) {
        return;
    }

    for (size_t i = 0; i < len && out + 1 < dstsz; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[out++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }

    dst[out] = '\0';
}

/* ------------------------------------------------------------------ */
/* Neighbor table                                                     */
/* ------------------------------------------------------------------ */

/*
 * Look up a configured neighbor by router ID.
 * Returns NULL if that router is not one of our direct neighbors, which is how
 * we reject HELLOs from routers we are not supposed to be adjacent to.
 */
static struct neighbor *find_neighbor(struct router *r, int id)
{
    for (int i = 0; i < r->neighbor_count; i++) {
        if (r->neighbors[i].id == id) {
            return &r->neighbors[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Socket setup and sending                                           */
/* ------------------------------------------------------------------ */

/*
 * Create the UDP socket and bind it to 127.0.0.1:(BASE_PORT + id).
 * Returns the file descriptor, or -1 on failure.
 */
static int open_socket(int id)
{
    int fd;
    struct sockaddr_in addr;

    /* AF_INET + SOCK_DGRAM = IPv4 UDP. */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* memset first: sockaddr_in has padding (sin_zero) that must be zero. */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)(BASE_PORT + id)); /* host to network byte order */

    /* inet_pton returns 1 on success, 0 for a badly formed address, -1 on error. */
    if (inet_pton(AF_INET, LOCALHOST, &addr.sin_addr) != 1) {
        fprintf(stderr, "inet_pton: cannot parse address %s\n", LOCALHOST);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        /* Most commonly EADDRINUSE: another copy of this router is running. */
        perror("bind");
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * Send one message to a router ID. UDP is connectionless, so we simply address
 * each datagram with sendto() rather than keeping per-neighbor connections.
 * A failure here is logged but never fatal: losing a HELLO is normal, and the
 * dead interval is what decides whether a neighbor is really gone.
 */
static void send_to_router(struct router *r, int dest_id, const char *msg)
{
    struct sockaddr_in addr;
    size_t len = strlen(msg);
    ssize_t sent;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)(BASE_PORT + dest_id));

    if (inet_pton(AF_INET, LOCALHOST, &addr.sin_addr) != 1) {
        fprintf(stderr, "[R%d] inet_pton failed for %s\n", r->id, LOCALHOST);
        return;
    }

    sent = sendto(r->sock, msg, len, 0, (struct sockaddr *)&addr, sizeof(addr));
    if (sent < 0) {
        /* ECONNREFUSED is expected on localhost when the peer process is not
         * running: the kernel turns the ICMP port-unreachable into an error on
         * our next send. It is not an error we can or should act on. */
        if (errno != ECONNREFUSED) {
            perror("sendto");
        }
    } else if ((size_t)sent != len) {
        fprintf(stderr, "[R%d] short send to R%d: %zd of %zu bytes\n",
                r->id, dest_id, sent, len);
    }
}

/*
 * Send a HELLO to every configured neighbor, whether or not we think it is up.
 * We keep greeting down neighbors because that is how a restarted or recovered
 * neighbor gets discovered again.
 */
static void send_hellos(struct router *r)
{
    char msg[MAX_MSG];
    int n;

    n = snprintf(msg, sizeof(msg), "HELLO %d", r->id);
    if (n < 0 || (size_t)n >= sizeof(msg)) {
        fprintf(stderr, "[R%d] HELLO did not fit in %zu bytes\n", r->id, sizeof(msg));
        return;
    }

    for (int i = 0; i < r->neighbor_count; i++) {
        send_to_router(r, r->neighbors[i].id, msg);
        if (r->verbose) {
            printf("[R%d] sent HELLO to %d\n", r->id, r->neighbors[i].id);
        }
    }
}

/* ------------------------------------------------------------------ */
/* LSA origination and flooding                                       */
/* ------------------------------------------------------------------ */

/*
 * Render an LSA as the on-the-wire text form:
 *     LSA <origin> <seq> <count> <nbr>:<cost> <nbr>:<cost> ...
 * for example "LSA 2 7 3 1:1 3:1 4:1".
 *
 * Returns 1 on success, 0 if the text would not fit in buf. snprintf() returns
 * the length it *wanted* to write, so comparing that against the remaining
 * space is how we detect truncation instead of silently sending a half message.
 */
static int format_lsa(char *buf, size_t sz, int origin, unsigned int seq,
                      const struct neighbor_link *links, int count)
{
    size_t used;
    int n;

    n = snprintf(buf, sz, "LSA %d %u %d", origin, seq, count);
    if (n < 0 || (size_t)n >= sz) {
        fprintf(stderr, "format_lsa: header did not fit\n");
        return 0;
    }
    used = (size_t)n;

    for (int i = 0; i < count; i++) {
        n = snprintf(buf + used, sz - used, " %d:%d", links[i].id, links[i].cost);
        if (n < 0 || (size_t)n >= sz - used) {
            fprintf(stderr, "format_lsa: link list did not fit\n");
            return 0;
        }
        used += (size_t)n;
    }

    return 1;
}

/*
 * Send a message to every neighbor that is currently up, skipping exclude_id.
 *
 * exclude_id is the neighbor an LSA arrived from: sending it straight back
 * would be pure waste, since that neighbor already has it. Pass 0 to flood to
 * everyone (0 is not a valid router ID).
 *
 * Down neighbors are skipped because there is nobody listening on the far end.
 */
static void flood(struct router *r, const char *msg, int exclude_id)
{
    for (int i = 0; i < r->neighbor_count; i++) {
        struct neighbor *n = &r->neighbors[i];

        if (!n->up || n->id == exclude_id) {
            continue;
        }
        send_to_router(r, n->id, msg);
    }
}

/*
 * Produce a fresh LSA describing our own links and flood it.
 *
 * Only neighbors that are currently up are listed. That is the whole mechanism
 * by which a failure propagates: when a link dies we advertise a smaller link
 * set, the two-way check in other routers' databases then fails for that link,
 * and their Dijkstra runs stop using it.
 */
static void originate_lsa(struct router *r, long long now)
{
    struct neighbor_link links[MAX_NEIGHBORS];
    char msg[MAX_MSG];
    int count = 0;

    for (int i = 0; i < r->neighbor_count; i++) {
        if (r->neighbors[i].up) {
            links[count].id = r->neighbors[i].id;
            links[count].cost = r->neighbors[i].cost;
            count++;
        }
    }

    /* A strictly increasing sequence number is what lets every other router
     * tell our new LSA apart from an older copy still circulating. */
    r->own_seq++;

    /* Our own LSA goes into the same database as everyone else's, so that
     * spf.c can treat the local router no differently from a remote one. */
    if (lsdb_install(&r->lsdb, r->id, r->own_seq, links, count, now)) {
        r->routes_dirty = 1;
    }

    if (format_lsa(msg, sizeof(msg), r->id, r->own_seq, links, count)) {
        flood(r, msg, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Route calculation                                                  */
/* ------------------------------------------------------------------ */

/*
 * Re-run Dijkstra and print the result, but only if the result is different
 * from what we printed last time.
 *
 * Suppressing unchanged tables is what makes the logs useful: what gets
 * printed is the sequence of convergence events, not one table per received
 * packet. The very first table is also suppressed while it is still empty,
 * since a router that has not yet heard from anybody has nothing to say.
 */
static void recompute_routes(struct router *r)
{
    spf_compute(&r->lsdb, r->id, &r->table);

    if (!r->have_printed) {
        if (r->table.count == 0) {
            return;
        }
    } else if (spf_tables_equal(&r->table, &r->printed)) {
        return;
    }

    spf_print_table(&r->table, stdout);

    r->printed = r->table;
    r->have_printed = 1;
}

/* ------------------------------------------------------------------ */
/* Receiving                                                          */
/* ------------------------------------------------------------------ */

/*
 * Work out which router sent a datagram from its source port, using the same
 * port = BASE_PORT + id convention we use when sending.
 * Returns 0 if the port does not correspond to a legal router ID.
 */
static int router_id_from_addr(const struct sockaddr_in *addr)
{
    int id = (int)ntohs(addr->sin_port) - BASE_PORT;

    if (id < 1 || id > MAX_ROUTER_ID) {
        return 0;
    }
    return id;
}

/*
 * Handle a HELLO. The sender ID carried in the message must match the router
 * ID implied by the source port, and must be one of our configured neighbors.
 * Anything else is dropped so a stray or spoofed packet cannot create an
 * adjacency that the topology file does not describe.
 */
static void handle_hello(struct router *r, int claimed_id, int from_id, long long now)
{
    struct neighbor *n;

    if (claimed_id != from_id) {
        printf("[R%d] ignoring HELLO claiming to be R%d but sent from R%d\n",
               r->id, claimed_id, from_id);
        return;
    }

    n = find_neighbor(r, claimed_id);
    if (n == NULL) {
        printf("[R%d] ignoring HELLO from R%d which is not a configured neighbor\n",
               r->id, claimed_id);
        return;
    }

    /* Refresh the liveness timer first, then report a transition if any. */
    n->last_hello_ms = now;

    if (!n->up) {
        n->up = 1;
        printf("[R%d] neighbor %d UP\n", r->id, n->id);
        r->neighbor_changed = 1;    /* our link state changed: re-originate */
    }

    if (r->verbose) {
        printf("[R%d] received HELLO from %d\n", r->id, n->id);
    }
}

/*
 * Parse a whole string as a decimal int. Returns 1 on success, 0 on failure.
 * strtol plus an explicit end-pointer check is used instead of atoi() so that
 * "12x", "" and out-of-range values are all rejected rather than silently
 * producing a number.
 */
static int parse_int(const char *s, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);

    if (s == end || *end != '\0') {
        return 0;               /* empty, or trailing junk */
    }
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return 0;               /* out of range for a long, or for an int */
    }

    *out = (int)v;
    return 1;
}

/*
 * Parse a whole string as a decimal unsigned int, used for sequence numbers.
 * strtoul() happily accepts "-1" and wraps it to a huge value, so we reject a
 * leading minus sign explicitly before calling it.
 */
static int parse_uint(const char *s, unsigned int *out)
{
    char *end;
    unsigned long v;

    if (s[0] == '-') {
        return 0;
    }

    errno = 0;
    v = strtoul(s, &end, 10);

    if (s == end || *end != '\0') {
        return 0;
    }
    if (errno == ERANGE || v > UINT_MAX) {
        return 0;
    }

    *out = (unsigned int)v;
    return 1;
}

/*
 * Parse one "<nbr>:<cost>" field of an LSA. Writes into tok (replacing the
 * colon with a NUL so the two halves become separate strings), which is fine
 * because tok points into the scratch receive buffer.
 * Returns 1 on success, 0 if the field is malformed or out of range.
 */
static int parse_link_field(char *tok, struct neighbor_link *out)
{
    char *colon = strchr(tok, ':');

    if (colon == NULL) {
        return 0;
    }
    *colon = '\0';

    if (!parse_int(tok, &out->id) || out->id < 1 || out->id > MAX_ROUTER_ID) {
        return 0;
    }
    if (!parse_int(colon + 1, &out->cost) ||
        out->cost < 1 || out->cost > MAX_LINK_COST) {
        return 0;
    }

    return 1;
}

/*
 * Handle a received LSA.
 *
 * `raw` is the datagram exactly as it arrived. If the LSA turns out to be news
 * we forward those original bytes rather than re-rendering them, so that the
 * message propagates unchanged across the network.
 */
static void handle_lsa(struct router *r, char *saveptr, const char *raw,
                       int from_id, long long now)
{
    struct neighbor_link links[MAX_NEIGHBORS];
    char *origin_tok, *seq_tok, *count_tok;
    unsigned int seq;
    int origin, count;

    origin_tok = strtok_r(NULL, " \t\r\n", &saveptr);
    seq_tok    = strtok_r(NULL, " \t\r\n", &saveptr);
    count_tok  = strtok_r(NULL, " \t\r\n", &saveptr);

    if (origin_tok == NULL || seq_tok == NULL || count_tok == NULL) {
        printf("[R%d] ignoring LSA with missing header fields from R%d\n",
               r->id, from_id);
        return;
    }

    if (!parse_int(origin_tok, &origin) || origin < 1 || origin > MAX_ROUTER_ID) {
        printf("[R%d] ignoring LSA with bad origin from R%d\n", r->id, from_id);
        return;
    }
    if (!parse_uint(seq_tok, &seq)) {
        printf("[R%d] ignoring LSA with bad sequence number from R%d\n", r->id, from_id);
        return;
    }
    if (!parse_int(count_tok, &count) || count < 0 || count > MAX_NEIGHBORS) {
        printf("[R%d] ignoring LSA with bad link count from R%d\n", r->id, from_id);
        return;
    }

    /* Read exactly `count` link fields: too few means a truncated message. */
    for (int i = 0; i < count; i++) {
        char *tok = strtok_r(NULL, " \t\r\n", &saveptr);

        if (tok == NULL || !parse_link_field(tok, &links[i])) {
            printf("[R%d] ignoring LSA with bad link field from R%d\n", r->id, from_id);
            return;
        }

        /* A router cannot have a link to itself. */
        if (links[i].id == origin) {
            printf("[R%d] ignoring LSA from R%d advertising a self-link\n",
                   r->id, from_id);
            return;
        }

        /* Nor two links to the same neighbor. */
        for (int j = 0; j < i; j++) {
            if (links[j].id == links[i].id) {
                printf("[R%d] ignoring LSA from R%d with a duplicate neighbor\n",
                       r->id, from_id);
                return;
            }
        }
    }

    /* Anything after the declared number of links means the message is wrong. */
    if (strtok_r(NULL, " \t\r\n", &saveptr) != NULL) {
        printf("[R%d] ignoring LSA with trailing fields from R%d\n", r->id, from_id);
        return;
    }

    /*
     * lsdb_install() makes the flooding decision: it returns non-zero only if
     * this LSA is genuinely newer than anything we hold. If it is old news we
     * do nothing at all, and the flood stops here.
     */
    if (lsdb_install(&r->lsdb, origin, seq, links, count, now)) {
        flood(r, raw, from_id);
        r->routes_dirty = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Data plane                                                         */
/* ------------------------------------------------------------------ */

/*
 * Handle a received data packet:
 *     DATA <src> <dst> <ttl> <payload>
 * for example "DATA 1 3 8 hello world".
 *
 * Everything above this point is control plane: it works out where things are.
 * This function is the data plane: given a packet, send it one hop closer. The
 * only thing it needs from all that machinery is a single lookup in the most
 * recently computed routing table, which is how the split works in real
 * routers too.
 *
 * `saveptr` continues the tokenization that handle_message() began, positioned
 * just after the DATA verb.
 */
static void handle_data(struct router *r, char *saveptr)
{
    char safe[MAX_MSG + 1];
    char msg[MAX_MSG];
    char *src_tok, *dst_tok, *ttl_tok, *payload;
    int src, dst, ttl, next_hop;
    size_t payload_len;

    src_tok = strtok_r(NULL, " \t\r\n", &saveptr);
    dst_tok = strtok_r(NULL, " \t\r\n", &saveptr);
    ttl_tok = strtok_r(NULL, " \t\r\n", &saveptr);

    if (src_tok == NULL || dst_tok == NULL || ttl_tok == NULL) {
        printf("[R%d] ignoring DATA with missing header fields\n", r->id);
        return;
    }

    if (!parse_int(src_tok, &src) || src < 1 || src > MAX_ROUTER_ID) {
        printf("[R%d] ignoring DATA with bad source\n", r->id);
        return;
    }
    if (!parse_int(dst_tok, &dst) || dst < 1 || dst > MAX_ROUTER_ID) {
        printf("[R%d] ignoring DATA with bad destination\n", r->id);
        return;
    }
    /* A ttl of 0 is legal to receive: it simply dies here. */
    if (!parse_int(ttl_tok, &ttl) || ttl < 0 || ttl > MAX_TTL) {
        printf("[R%d] ignoring DATA with bad ttl\n", r->id);
        return;
    }

    /*
     * The payload is everything left in the message, spaces included, so it is
     * taken as the remainder of the buffer rather than as another token.
     * strtok_r left saveptr just past the ttl token, so skip the separator.
     * An empty payload is accepted: the payload is opaque to us, and only the
     * header fields are worth rejecting a packet over.
     */
    payload = saveptr;
    while (*payload == ' ' || *payload == '\t') {
        payload++;
    }

    /* Trim a trailing newline, which a hand-typed or netcat-sent message may
     * carry. Without this it would end up inside the forwarded payload. */
    payload_len = strlen(payload);
    while (payload_len > 0 &&
           (payload[payload_len - 1] == '\n' || payload[payload_len - 1] == '\r')) {
        payload[--payload_len] = '\0';
    }

    /* We are the destination: the packet has arrived. */
    if (dst == r->id) {
        /* The payload came off the network, so strip control characters out of
         * it before logging. */
        sanitize(payload, payload_len, safe, sizeof(safe));
        printf("[R%d] delivered DATA from R%d: \"%s\" (ttl %d)\n",
               r->id, src, safe, ttl);
        return;
    }

    /*
     * Out of hops. Forwarding would decrement the ttl to zero, so the packet
     * dies here instead. This is the backstop against a packet circulating
     * forever: during the brief window while a failure is still propagating,
     * two routers can disagree about the next hop and bounce a packet between
     * themselves, and the ttl is what eventually ends that.
     */
    if (ttl <= 1) {
        printf("[R%d] dropped DATA %d->%d: ttl expired\n", r->id, src, dst);
        return;
    }

    /* One lookup in the table the control plane has already computed. */
    next_hop = spf_next_hop(&r->table, dst);
    if (next_hop < 0) {
        printf("[R%d] dropped DATA %d->%d: no route\n", r->id, src, dst);
        return;
    }

    ttl--;

    /* Rebuilding cannot overflow: only the ttl changed, and it got smaller. */
    if (snprintf(msg, sizeof(msg), "DATA %d %d %d %s", src, dst, ttl, payload)
            >= (int)sizeof(msg)) {
        printf("[R%d] dropped DATA %d->%d: message too long to forward\n",
               r->id, src, dst);
        return;
    }

    send_to_router(r, next_hop, msg);
    printf("[R%d] forwarding DATA %d->%d via R%d (ttl %d)\n",
           r->id, src, dst, next_hop, ttl);
}

/*
 * Dispatch one received message. buf is NUL-terminated and owned by the
 * caller; we are free to tokenize it in place.
 */
static void handle_message(struct router *r, char *buf, size_t len,
                           int from_id, long long now)
{
    char safe[MAX_MSG + 1];
    char raw[MAX_MSG + 1];
    char *saveptr = NULL;
    char *verb;

    /*
     * Keep a pristine copy before tokenizing. strtok_r() writes NUL bytes into
     * buf to terminate each token, which would destroy the message; a flooded
     * LSA has to be forwarded byte for byte exactly as it arrived.
     */
    memcpy(raw, buf, len + 1);

    /* strtok_r is the reentrant form of strtok: the saveptr replaces strtok's
     * hidden global state. Delimiters include \r and \n so a trailing newline
     * on a hand-typed test message does not become part of a token. */
    verb = strtok_r(buf, " \t\r\n", &saveptr);
    if (verb == NULL) {
        printf("[R%d] ignoring empty message\n", r->id);
        return;
    }

    /*
     * DATA is checked before the source port, deliberately.
     *
     * A data packet is traffic, not a protocol message. It can legitimately be
     * injected by anything, and send.sh uses netcat, which sends from an
     * ephemeral port that corresponds to no router at all. The src and dst
     * fields inside the message are what identify it, so there is nothing to
     * match the port against.
     */
    if (strcmp(verb, "DATA") == 0) {
        handle_data(r, saveptr);
        return;
    }

    /*
     * HELLO and LSA are control plane messages, and those we do attribute by
     * source port, so that a stray packet cannot invent an adjacency or inject
     * topology into the database.
     */
    if (from_id == 0) {
        sanitize(raw, len, safe, sizeof(safe));
        printf("[R%d] ignoring message from unknown source port: \"%.64s\"\n",
               r->id, safe);
        return;
    }

    if (strcmp(verb, "HELLO") == 0) {
        char *id_tok = strtok_r(NULL, " \t\r\n", &saveptr);
        int claimed_id;

        if (id_tok == NULL || !parse_int(id_tok, &claimed_id) ||
            claimed_id < 1 || claimed_id > MAX_ROUTER_ID) {
            printf("[R%d] ignoring malformed HELLO from R%d\n", r->id, from_id);
            return;
        }
        if (strtok_r(NULL, " \t\r\n", &saveptr) != NULL) {
            printf("[R%d] ignoring HELLO with extra fields from R%d\n", r->id, from_id);
            return;
        }

        handle_hello(r, claimed_id, from_id, now);
        return;
    }

    if (strcmp(verb, "LSA") == 0) {
        handle_lsa(r, saveptr, raw, from_id, now);
        return;
    }

    /* The verb came off the network, so strip control characters and cap the
     * length before letting it anywhere near a log file. */
    sanitize(verb, strlen(verb), safe, sizeof(safe));
    printf("[R%d] ignoring message with unknown type \"%.32s\" from R%d\n",
           r->id, safe, from_id);
}

/*
 * Read exactly one datagram and process it. Called only after poll() has told
 * us the socket is readable, so recvfrom() will not block.
 */
static void receive_one(struct router *r, long long now)
{
    char buf[MAX_MSG + 1];      /* +1 so we always have room for a NUL */
    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    ssize_t got;

    got = recvfrom(r->sock, buf, MAX_MSG, 0, (struct sockaddr *)&src, &srclen);
    if (got < 0) {
        /* EINTR: a signal arrived mid-call. EAGAIN: spurious readability.
         * Neither is a real error, so we just come back next time round. */
        if (errno != EINTR && errno != EAGAIN) {
            perror("recvfrom");
        }
        return;
    }

    /* UDP payloads are not NUL-terminated on the wire; we terminate so the
     * rest of the code can treat the datagram as a C string. */
    buf[got] = '\0';

    handle_message(r, buf, (size_t)got, router_id_from_addr(&src), now);
}

/* ------------------------------------------------------------------ */
/* Timers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Declare any neighbor that has been silent for longer than the dead interval
 * to be down, and flag the change so we re-originate our LSA.
 */
static void check_dead_neighbors(struct router *r, long long now)
{
    for (int i = 0; i < r->neighbor_count; i++) {
        struct neighbor *n = &r->neighbors[i];

        if (n->up && now - n->last_hello_ms > DEAD_INTERVAL_MS) {
            n->up = 0;
            printf("[R%d] neighbor %d DOWN\n", r->id, n->id);
            r->neighbor_changed = 1;
        }
    }
}

/*
 * How long poll() may sleep: the time until the earliest pending timer.
 *
 * This is deliberately not a fixed sleep. A fixed sleep would either burn CPU
 * or make us react late; computing the real deadline means we wake exactly
 * when there is work to do and block the rest of the time.
 */
static int poll_timeout_ms(const struct router *r, long long now,
                           long long next_hello, long long next_refresh,
                           long long deadline)
{
    long long next = next_hello;
    long long expiry;
    long long delta;

    if (next_refresh < next) {
        next = next_refresh;
    }

    /* With --run-for, the moment we must exit is just another timer.
     * A negative deadline means --run-for was not given. */
    if (deadline >= 0 && deadline < next) {
        next = deadline;
    }

    /* An up neighbor has a deadline: the moment it becomes declarable dead. */
    for (int i = 0; i < r->neighbor_count; i++) {
        const struct neighbor *n = &r->neighbors[i];

        if (n->up) {
            long long deadline = n->last_hello_ms + DEAD_INTERVAL_MS + 1;
            if (deadline < next) {
                next = deadline;
            }
        }
    }

    /* The soonest a stored LSA can age out, if any can. */
    expiry = lsdb_next_expiry_ms(&r->lsdb, LSA_MAX_AGE_MS);
    if (expiry >= 0 && expiry < next) {
        next = expiry;
    }

    delta = next - now;
    if (delta < 0) {
        delta = 0;              /* already due: poll must not block */
    }

    return (int)delta;
}

/*
 * Advance a periodic timer that has just fired. Adding the interval (rather
 * than setting now + interval) keeps the period from drifting, but if we were
 * descheduled for longer than one interval we skip ahead instead of firing
 * repeatedly to catch up.
 */
static long long advance_timer(long long fired_at, long long now, long long interval)
{
    long long next = fired_at + interval;

    if (next <= now) {
        next = now + interval;
    }
    return next;
}

/* ------------------------------------------------------------------ */
/* Command line                                                       */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <id> [topology_file] [--run-for SECONDS] [-v]\n"
            "  <id>              this router's ID, 1..%d\n"
            "  topology_file     link list, default \"topology.txt\"\n"
            "  --run-for SECONDS exit cleanly after SECONDS, printing a final table\n"
            "  -v                also log every HELLO sent and received\n",
            prog, MAX_ROUTER_ID);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct router r;
    const char *topo_path = "topology.txt";
    int have_id = 0;
    int run_for_s = 0;      /* 0 means run until a signal arrives */
    int n;

    /*
     * Line-buffer stdout. When stdout is a file or a pipe (which is how run.sh
     * and the tests use it) the C library would otherwise buffer 4 KiB at a
     * time, so logs would appear in bursts and a killed process would lose its
     * output entirely. _IOLBF flushes on every newline.
     */
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        perror("setvbuf");
        return EXIT_FAILURE;
    }

    memset(&r, 0, sizeof(r));
    r.sock = -1;

    /* Arguments: one required positional ID, one optional positional topology
     * path, and flags that may appear anywhere. */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-v") == 0) {
            r.verbose = 1;
        } else if (strcmp(arg, "--run-for") == 0) {
            /* Takes a value in the next argv slot. */
            if (i + 1 >= argc) {
                fprintf(stderr, "--run-for needs a number of seconds\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            i++;
            if (!parse_int(argv[i], &run_for_s) || run_for_s < 1) {
                fprintf(stderr, "--run-for \"%s\" must be a positive integer\n", argv[i]);
                return EXIT_FAILURE;
            }
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "unknown option \"%s\"\n", arg);
            usage(argv[0]);
            return EXIT_FAILURE;
        } else if (!have_id) {
            if (!parse_int(arg, &r.id) || r.id < 1 || r.id > MAX_ROUTER_ID) {
                fprintf(stderr, "router ID \"%s\" must be an integer 1..%d\n",
                        arg, MAX_ROUTER_ID);
                return EXIT_FAILURE;
            }
            have_id = 1;
        } else {
            topo_path = arg;
        }
    }

    if (!have_id) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /*
     * Learn our own links, and only our own links, from the topology file.
     * The loader deals in plain struct neighbor_link (an ID and a cost); we
     * copy those into our richer struct neighbor, which adds the liveness
     * fields. They start zeroed, meaning "down, never heard from".
     */
    struct neighbor_link links[MAX_NEIGHBORS];

    n = topology_load_own_links(topo_path, r.id, links, MAX_NEIGHBORS);
    if (n < 0) {
        return EXIT_FAILURE;
    }

    for (int i = 0; i < n; i++) {
        r.neighbors[i].id = links[i].id;
        r.neighbors[i].cost = links[i].cost;
        r.neighbors[i].up = 0;
        r.neighbors[i].last_hello_ms = 0;
    }
    r.neighbor_count = n;

    printf("[R%d] starting with %d configured neighbor(s):", r.id, r.neighbor_count);
    for (int i = 0; i < r.neighbor_count; i++) {
        printf(" %d(cost %d)", r.neighbors[i].id, r.neighbors[i].cost);
    }
    printf("\n");

    /*
     * Catch the two signals that normally mean "stop": Ctrl+C from a terminal
     * and the default kill. Both are turned into a clean exit that still
     * prints the final table. SIGKILL cannot be caught, which is exactly why
     * the failure test uses it to make a router die silently.
     */
    if (install_signal_handler(SIGINT) != 0 ||
        install_signal_handler(SIGTERM) != 0) {
        return EXIT_FAILURE;
    }

    r.sock = open_socket(r.id);
    if (r.sock < 0) {
        return EXIT_FAILURE;
    }
    printf("[R%d] listening on %s:%d\n", r.id, LOCALHOST, BASE_PORT + r.id);

    lsdb_init(&r.lsdb, r.id);

    /*
     * Originate our first LSA straight away, before any neighbor has answered.
     * It lists no links, because nothing is up yet, but it puts our own entry
     * in the database so the rest of the code never has to special-case an
     * empty database.
     */
    long long start = now_ms();
    originate_lsa(&r, start);

    /*
     * The event loop. Every pass does the same three things:
     *   1. work out how long until the next timer is due,
     *   2. block in poll() until either a packet arrives or that moment comes,
     *   3. service whichever of the two happened.
     */
    long long next_hello = start;                      /* greet immediately */
    long long next_refresh = start + LSA_REFRESH_MS;   /* we just originated */

    /* With --run-for, the instant at which we stop. -1 means no deadline. */
    long long deadline = (run_for_s > 0) ? start + (long long)run_for_s * 1000 : -1;

    while (!g_shutdown) {
        struct pollfd pfd;
        long long now = now_ms();
        int timeout = poll_timeout_ms(&r, now, next_hello, next_refresh, deadline);
        int ready;

        pfd.fd = r.sock;
        pfd.events = POLLIN;           /* wake us when there is data to read */
        pfd.revents = 0;

        ready = poll(&pfd, 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                /* A signal arrived. Go back to the top so the loop condition
                 * can see g_shutdown; if it was some other signal we simply
                 * carry on. */
                continue;
            }
            perror("poll");
            close(r.sock);
            return EXIT_FAILURE;
        }

        /* Re-read the clock: poll() may have slept for the full timeout. */
        now = now_ms();

        if (ready > 0 && (pfd.revents & POLLIN)) {
            receive_one(&r, now);
        }

        if (now >= next_hello) {
            send_hellos(&r);
            next_hello = advance_timer(next_hello, now, HELLO_INTERVAL_MS);
        }

        check_dead_neighbors(&r, now);

        /*
         * Re-originate our LSA either because something changed or because the
         * refresh timer came due. Doing it immediately on a change is what
         * makes reconvergence quick; the periodic refresh is what stops other
         * routers aging our LSA out while we are still alive.
         */
        if (r.neighbor_changed) {
            r.neighbor_changed = 0;
            originate_lsa(&r, now);
            next_refresh = now + LSA_REFRESH_MS;
        } else if (now >= next_refresh) {
            originate_lsa(&r, now);
            next_refresh = advance_timer(next_refresh, now, LSA_REFRESH_MS);
        }

        /* Forget LSAs whose origin has gone quiet for too long. */
        if (lsdb_age_out(&r.lsdb, now, LSA_MAX_AGE_MS) > 0) {
            r.routes_dirty = 1;
        }

        /*
         * Recompute once per pass rather than once per change. A single pass
         * can install several LSAs, and running Dijkstra once at the end of it
         * gives the same answer for less work and fewer half-converged tables
         * in the log.
         */
        if (r.routes_dirty) {
            r.routes_dirty = 0;
            recompute_routes(&r);
        }

        /* --run-for: checked last, so the final pass is fully serviced before
         * we leave and print the final table. */
        if (deadline >= 0 && now >= deadline) {
            break;
        }
    }

    /*
     * Clean shutdown. We recompute rather than reuse the last printed table,
     * so the final output reflects the database as it stands right now even if
     * the last change was suppressed as "no visible difference".
     *
     * The FINAL and END markers delimit the block the tests parse out of the
     * process's stdout.
     */
    printf("FINAL\n");
    spf_compute(&r.lsdb, r.id, &r.table);
    spf_print_table(&r.table, stdout);
    printf("END\n");

    if (close(r.sock) != 0) {
        perror("close");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
