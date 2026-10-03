/*
 * lsdb.h - the link-state database.
 *
 * Every router floods one LSA ("link state advertisement") describing its own
 * working links. Each router stores the most recent LSA it has seen from every
 * origin. Once all the LSAs have spread, every router holds the same set of
 * LSAs, and that collection is a complete map of the network. Dijkstra then
 * runs over that map locally.
 *
 * This file owns three jobs:
 *   1. storing one LSA per origin, keyed by sequence number,
 *   2. deciding whether an arriving LSA is new enough to be worth flooding,
 *   3. aging out LSAs whose origin has stopped refreshing them,
 * plus the two-way link check that spf.c uses to build its graph.
 */
#ifndef LSDB_H
#define LSDB_H

#include "topology.h"

/*
 * One stored LSA: router `origin` claims these `count` working links as of
 * sequence number `seq`.
 */
struct lsa {
    int valid;                              /* 0 = this slot holds no LSA */
    int origin;                             /* router that produced the LSA */
    unsigned int seq;                       /* higher means newer */
    int count;                              /* number of entries in links[] */
    struct neighbor_link links[MAX_NEIGHBORS];
    long long recv_ms;                      /* when we stored it, CLOCK_MONOTONIC */
};

struct lsdb {
    int self_id;                            /* so we can log with our own prefix */

    /* One slot per possible origin, indexed directly by router ID. Our own
     * LSA lives in entries[self_id] like everybody else's; there is nothing
     * special about it except that it never ages out. */
    struct lsa entries[MAX_ROUTER_ID + 1];

    /*
     * Every router ID we have ever heard of, as an origin or as somebody's
     * listed neighbor. This is deliberately sticky: it is never cleared.
     *
     * It exists so that a destination which becomes unreachable can still be
     * printed as "unreachable" rather than silently vanishing from the table.
     * When a router dies, its LSA ages out and no surviving LSA mentions it,
     * so without this set we would simply forget the destination existed.
     */
    int known[MAX_ROUTER_ID + 1];
};

/* Prepare an empty database belonging to router self_id. */
void lsdb_init(struct lsdb *db, int self_id);

/*
 * Consider an LSA for installation. This is the single place the flooding
 * decision is made.
 *
 * Install it if we hold no LSA from this origin, or if seq is greater than the
 * one we hold; in that case log the arrival and return 1, meaning "this was
 * news: flood it onward and recompute routes".
 *
 * Otherwise return 0 and do nothing. Returning 0 for anything we have already
 * seen is what stops flooding from looping forever: a copy that comes back
 * round the network is simply not re-sent.
 */
int lsdb_install(struct lsdb *db, int origin, unsigned int seq,
                 const struct neighbor_link *links, int count, long long now_ms);

/*
 * Discard every stored LSA, other than our own, that has not been refreshed
 * within max_age_ms. Our own LSA is exempt because we are the authority on it
 * and we re-originate it on a timer anyway.
 *
 * Returns the number of LSAs removed, so the caller knows whether to
 * recompute routes.
 */
int lsdb_age_out(struct lsdb *db, long long now_ms, long long max_age_ms);

/*
 * The earliest moment at which some stored LSA will be old enough to expire,
 * or -1 if nothing can expire. Used to size the poll() timeout.
 */
long long lsdb_next_expiry_ms(const struct lsdb *db, long long max_age_ms);

/*
 * Cost of the link between a and b, or -1 if there is no usable link.
 *
 * This applies the two-way check that real OSPF also performs: the link only
 * counts if a's LSA lists b AND b's LSA lists a. One-sided information is
 * stale or describes a half-broken link, and trusting it would produce routes
 * through links that cannot carry traffic. The cost returned is the one a
 * advertises.
 */
int lsdb_link_cost(const struct lsdb *db, int a, int b);

/* Have we ever heard of this router ID? See struct lsdb::known. */
int lsdb_is_known(const struct lsdb *db, int id);

#endif /* LSDB_H */
