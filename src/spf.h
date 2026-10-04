/*
 * spf.h - shortest path first: Dijkstra over the link-state database.
 *
 * This is the step that turns "a map of the network" into "what do I do with a
 * packet for router N". Because every router ends up with the same LSAs, every
 * router runs this same calculation over the same map and they all reach
 * consistent, loop-free answers without ever exchanging a route.
 */
#ifndef SPF_H
#define SPF_H

#include <stdio.h>

#include "lsdb.h"

/* Sentinel stored in struct route::cost when a destination has no path. */
#define SPF_INFINITE_COST (-1)

/*
 * One row of the routing table.
 *
 * next_hop is the neighbor to send the packet to: the first router after us on
 * the shortest path. That single number is all a real forwarding plane needs;
 * the full path is kept only so the output is explainable.
 */
struct route {
    int dest;                        /* destination router ID */
    int next_hop;                    /* first hop toward dest, 0 if unreachable */
    int cost;                        /* total path cost, SPF_INFINITE_COST if none */
    int path[MAX_ROUTER_ID + 1];     /* self first, dest last */
    int path_len;                    /* entries used in path[], 0 if unreachable */
};

/*
 * A computed routing table: one row per router we have ever heard of, except
 * ourselves, sorted by destination ID.
 */
struct routing_table {
    int self_id;
    struct route routes[MAX_ROUTER_ID];
    int count;
};

/*
 * Run Dijkstra from self_id over the graph implied by db and fill in out.
 * Always succeeds; destinations with no usable path get SPF_INFINITE_COST.
 */
void spf_compute(const struct lsdb *db, int self_id, struct routing_table *out);

/*
 * Do two tables describe the same routing? Used to print the table only when
 * it actually changes, instead of on every recalculation.
 */
int spf_tables_equal(const struct routing_table *a, const struct routing_table *b);

/* Print the table in the project's fixed column format. */
void spf_print_table(const struct routing_table *t, FILE *out);

/*
 * The next hop toward dst, or -1 if dst is unreachable or absent from the
 * table.
 *
 * This is the whole interface the data plane needs. Forwarding a packet is one
 * lookup in an already computed table, which is exactly the split real routers
 * make: the control plane works out the routes slowly and occasionally, the
 * data plane just reads the answer.
 */
int spf_next_hop(const struct routing_table *t, int dst);

#endif /* SPF_H */
