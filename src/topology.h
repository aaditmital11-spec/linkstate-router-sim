/*
 * topology.h - reading this router's own directly connected links.
 *
 * Design rule that matters for the whole project: a router is only allowed to
 * read the topology file to discover the links it is physically attached to.
 * Everything else about the network must be learned over the wire from LSA
 * messages. topology_load_own_links() is what enforces that rule: it throws
 * away every line of the file that does not mention this router's own ID.
 */
#ifndef TOPOLOGY_H
#define TOPOLOGY_H

/* Router IDs are 1..MAX_ROUTER_ID. We index arrays directly by router ID, so
 * most arrays are sized MAX_ROUTER_ID + 1 and slot 0 is left unused. That
 * wastes one slot and buys us code with no "id - 1" arithmetic to get wrong. */
#define MAX_ROUTER_ID 16

/* A router can have at most one link to each other router, so it can never
 * have more neighbors than there are other routers. */
#define MAX_NEIGHBORS MAX_ROUTER_ID

/* Upper bound on a link cost. Bounding it keeps every LSA message comfortably
 * inside the 512 byte limit and keeps path costs far away from int overflow. */
#define MAX_LINK_COST 65535

/*
 * One end of a link, from the point of view of whoever is holding it.
 * If router 1 has a link to router 2 with cost 5, then router 1 stores
 * { .id = 2, .cost = 5 }. The link is undirected in the topology file, but
 * each router only ever records the far end.
 */
struct neighbor_link {
    int id;   /* router ID at the far end of the link, 1..MAX_ROUTER_ID */
    int cost; /* cost of traversing the link, always >= 1 */
};

/*
 * Parse the topology file and return ONLY the links that involve self_id.
 *
 * File format, one undirected link per line:
 *     <routerA> <routerB> <cost>
 * Blank lines and lines whose first non-space character is '#' are ignored.
 * Malformed lines are reported on stderr and skipped rather than being fatal,
 * so one bad line cannot stop the simulation.
 *
 *   path     path to the topology file
 *   self_id  the ID of the router doing the reading
 *   out      caller-provided array that receives the neighbors
 *   max_out  capacity of out, in elements
 *
 * Returns the number of neighbors written to out, or -1 on error (file could
 * not be opened, or the file lists more neighbors for us than max_out).
 */
int topology_load_own_links(const char *path, int self_id,
                            struct neighbor_link *out, int max_out);

#endif /* TOPOLOGY_H */
