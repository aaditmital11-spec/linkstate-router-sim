/*
 * spf.c - Dijkstra's shortest path first algorithm, and table printing.
 *
 * Plain O(N^2) array-based Dijkstra. With at most 16 routers there is no point
 * in a binary heap: the arrays are trivial to read in a debugger and trivial
 * to explain, and that matters more here than asymptotic speed.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "spf.h"

/*
 * Buffer for the rendered path string, e.g. "1 -> 2 -> 3". Worst case is 16
 * two-digit IDs joined by 15 four-character separators, so 128 bytes is
 * generous.
 */
#define PATH_STR_MAX 128

/* Column layout. These widths produce exactly the required output:
 *   "Destination" is 11 characters, padded to 14, giving three spaces, and so
 *   on for the other headings. Keeping them as one format string means the
 *   header and the rows can never drift apart. */
#define ROW_FORMAT "%-14s%-11s%-7s%s\n"

/*
 * Pick the unvisited router with the smallest known distance.
 *
 * Scanning IDs in ascending order with a strict "<" comparison means that when
 * two routers are equally far away we always settle the lower ID first. That
 * is what makes the output deterministic instead of depending on array order.
 *
 * Returns 0 when every remaining router is unreachable.
 */
static int pick_closest_unvisited(const int *dist, const int *visited)
{
    int best_id = 0;
    int best_dist = INT_MAX;

    for (int id = 1; id <= MAX_ROUTER_ID; id++) {
        if (!visited[id] && dist[id] < best_dist) {
            best_dist = dist[id];
            best_id = id;
        }
    }

    return best_id;
}

/*
 * Walk the prev[] chain backwards from dest to self and store the path, in
 * forward order, into rt. Returns 1 on success, 0 if the chain is broken
 * (which should be impossible, but a corrupt chain must not loop forever or
 * run off the end of the array).
 */
static int build_path(struct route *rt, const int *prev, int self_id, int dest)
{
    int reverse[MAX_ROUTER_ID + 1];
    int len = 0;
    int cur = dest;

    /* Each step moves strictly closer to self along a shortest path, so at
     * most MAX_ROUTER_ID hops can be taken before we must arrive. */
    while (cur != self_id) {
        if (cur < 1 || cur > MAX_ROUTER_ID || len >= MAX_ROUTER_ID) {
            return 0;
        }
        reverse[len++] = cur;
        cur = prev[cur];
    }
    reverse[len++] = self_id;

    /* reverse[] runs dest..self, so copy it out backwards to get self..dest. */
    rt->path_len = len;
    for (int i = 0; i < len; i++) {
        rt->path[i] = reverse[len - 1 - i];
    }

    /* The next hop is the router immediately after us. path[0] is always us,
     * so a path of length 1 would mean dest == self, which we never ask for. */
    rt->next_hop = (len >= 2) ? rt->path[1] : 0;

    return 1;
}

void spf_compute(const struct lsdb *db, int self_id, struct routing_table *out)
{
    /* Indexed by router ID; slot 0 is unused, as everywhere else. */
    int dist[MAX_ROUTER_ID + 1];
    int prev[MAX_ROUTER_ID + 1];
    int visited[MAX_ROUTER_ID + 1];

    memset(out, 0, sizeof(*out));
    out->self_id = self_id;

    for (int id = 0; id <= MAX_ROUTER_ID; id++) {
        dist[id] = INT_MAX;      /* INT_MAX stands for "no path known yet" */
        prev[id] = 0;            /* 0 means "no predecessor" */
        visited[id] = 0;
    }

    /* We are zero cost away from ourselves; everything grows out from here. */
    dist[self_id] = 0;

    for (;;) {
        int u = pick_closest_unvisited(dist, visited);

        if (u == 0) {
            /* Every router we can reach has been settled. Whatever is still
             * at INT_MAX is genuinely unreachable. */
            break;
        }

        /*
         * u's distance is now final. This is the Dijkstra invariant: because
         * every cost is positive, no path discovered later can reach u more
         * cheaply than the cheapest one found so far.
         */
        visited[u] = 1;

        for (int v = 1; v <= MAX_ROUTER_ID; v++) {
            int link_cost;
            int through_u;

            if (visited[v]) {
                continue;
            }

            /* The only source of edges is the database, and it only reports a
             * link when both ends advertise each other. A one-sided link is
             * invisible to Dijkstra and therefore cannot be routed over. */
            link_cost = lsdb_link_cost(db, u, v);
            if (link_cost < 0) {
                continue;
            }

            /* dist[u] is finite because u was selected, so this cannot
             * overflow given the bound on link costs. */
            through_u = dist[u] + link_cost;

            /*
             * Relax the edge. The second clause is the tie-break: when two
             * equal-cost paths exist we keep the one whose predecessor has the
             * lower ID, so the printed path is the same on every run.
             */
            if (through_u < dist[v] ||
                (through_u == dist[v] && u < prev[v])) {
                dist[v] = through_u;
                prev[v] = u;
            }
        }
    }

    /*
     * Turn the distance array into table rows.
     *
     * We list every router we have ever heard of rather than only the
     * reachable ones, so that a destination which has just become unreachable
     * is reported as such instead of quietly disappearing.
     */
    for (int dest = 1; dest <= MAX_ROUTER_ID; dest++) {
        struct route *rt;

        if (dest == self_id || !lsdb_is_known(db, dest)) {
            continue;
        }

        rt = &out->routes[out->count];
        memset(rt, 0, sizeof(*rt));
        rt->dest = dest;

        if (dist[dest] == INT_MAX || !build_path(rt, prev, self_id, dest)) {
            rt->cost = SPF_INFINITE_COST;
            rt->next_hop = 0;
            rt->path_len = 0;
        } else {
            rt->cost = dist[dest];
        }

        out->count++;
    }
}

int spf_tables_equal(const struct routing_table *a, const struct routing_table *b)
{
    if (a->self_id != b->self_id || a->count != b->count) {
        return 0;
    }

    for (int i = 0; i < a->count; i++) {
        const struct route *ra = &a->routes[i];
        const struct route *rb = &b->routes[i];

        if (ra->dest != rb->dest || ra->next_hop != rb->next_hop ||
            ra->cost != rb->cost || ra->path_len != rb->path_len) {
            return 0;
        }

        /* Two paths of the same cost can still differ, and the path is part of
         * what we print, so it is part of what counts as a change. */
        for (int j = 0; j < ra->path_len; j++) {
            if (ra->path[j] != rb->path[j]) {
                return 0;
            }
        }
    }

    return 1;
}

int spf_next_hop(const struct routing_table *t, int dst)
{
    for (int i = 0; i < t->count; i++) {
        if (t->routes[i].dest != dst) {
            continue;
        }

        /* The row exists but has no path. We still return -1 rather than the
         * stored next_hop, which is 0 for an unreachable destination. */
        if (t->routes[i].cost == SPF_INFINITE_COST) {
            return -1;
        }

        return t->routes[i].next_hop;
    }

    /* Not in the table at all: a router we have never heard of. */
    return -1;
}

/*
 * Render a path as "1 -> 2 -> 3" into buf. Truncation is impossible given
 * PATH_STR_MAX and the 16 router limit, but the return value of snprintf is
 * still checked rather than assumed.
 */
static void format_path(const struct route *rt, char *buf, size_t sz)
{
    size_t used = 0;

    buf[0] = '\0';

    for (int i = 0; i < rt->path_len; i++) {
        int n = snprintf(buf + used, sz - used, "%s%d",
                         (i == 0) ? "" : " -> ", rt->path[i]);

        if (n < 0 || (size_t)n >= sz - used) {
            return;             /* leave whatever fitted, NUL-terminated */
        }
        used += (size_t)n;
    }
}

void spf_print_table(const struct routing_table *t, FILE *out)
{
    fprintf(out, "Router %d routing table\n", t->self_id);
    fprintf(out, ROW_FORMAT, "Destination", "Next Hop", "Cost", "Path");

    for (int i = 0; i < t->count; i++) {
        const struct route *rt = &t->routes[i];
        char dest_str[8];
        char next_hop_str[8];
        char cost_str[16];
        char path_str[PATH_STR_MAX];

        snprintf(dest_str, sizeof(dest_str), "%d", rt->dest);

        if (rt->cost == SPF_INFINITE_COST) {
            fprintf(out, ROW_FORMAT, dest_str, "-", "inf", "unreachable");
            continue;
        }

        snprintf(next_hop_str, sizeof(next_hop_str), "%d", rt->next_hop);
        snprintf(cost_str, sizeof(cost_str), "%d", rt->cost);
        format_path(rt, path_str, sizeof(path_str));

        fprintf(out, ROW_FORMAT, dest_str, next_hop_str, cost_str, path_str);
    }
}
