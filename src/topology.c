/*
 * topology.c - parse the topology file, keeping only our own links.
 *
 * See topology.h for the rule this file enforces: a router learns its own
 * attached links from the file and nothing more.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "topology.h"

/* Longest topology file line we accept. A line is three small integers, so
 * this is far more than we need and keeps fgets() simple. */
#define TOPO_LINE_MAX 256

/*
 * Is id a legal router ID? Router IDs are 1..MAX_ROUTER_ID because we index
 * arrays by ID and reserve slot 0 as "no router".
 */
static int id_is_valid(int id)
{
    return id >= 1 && id <= MAX_ROUTER_ID;
}

/*
 * Has this neighbor already been recorded? The topology file could list the
 * same pair twice; we keep the first entry and ignore later duplicates so the
 * neighbor list never contains the same router more than once.
 */
static int already_have_neighbor(const struct neighbor_link *out, int count, int id)
{
    for (int i = 0; i < count; i++) {
        if (out[i].id == id) {
            return 1;
        }
    }
    return 0;
}

int topology_load_own_links(const char *path, int self_id,
                            struct neighbor_link *out, int max_out)
{
    FILE *f;
    char line[TOPO_LINE_MAX];
    int count = 0;      /* neighbors found so far */
    int lineno = 0;     /* for error messages, 1-based */

    if (!id_is_valid(self_id)) {
        fprintf(stderr, "topology: router ID %d is out of range 1..%d\n",
                self_id, MAX_ROUTER_ID);
        return -1;
    }

    f = fopen(path, "r");
    if (f == NULL) {
        /* perror() appends the strerror() text for errno, e.g.
         * "topology.txt: No such file or directory". */
        perror(path);
        return -1;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        int a, b, cost;
        char extra;
        int fields;
        const char *p = line;

        lineno++;

        /* Skip leading whitespace so we can recognise indented comments. */
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        /* Blank line or comment: nothing to do. */
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') {
            continue;
        }

        /*
         * Read exactly three integers. The trailing " %c" is a guard: if
         * anything other than whitespace follows the cost, sscanf() matches it
         * and returns 4, which tells us the line has junk on the end. A clean
         * line returns 3 because the space before %c consumes the newline and
         * then the scan hits end of string.
         */
        fields = sscanf(p, "%d %d %d %c", &a, &b, &cost, &extra);
        if (fields != 3) {
            fprintf(stderr, "topology: %s:%d: malformed line, skipping\n",
                    path, lineno);
            continue;
        }

        if (!id_is_valid(a) || !id_is_valid(b)) {
            fprintf(stderr, "topology: %s:%d: router ID out of range 1..%d, skipping\n",
                    path, lineno, MAX_ROUTER_ID);
            continue;
        }

        if (cost < 1 || cost > MAX_LINK_COST) {
            fprintf(stderr, "topology: %s:%d: cost %d outside 1..%d, skipping\n",
                    path, lineno, cost, MAX_LINK_COST);
            continue;
        }

        if (a == b) {
            fprintf(stderr, "topology: %s:%d: self-loop on router %d, skipping\n",
                    path, lineno, a);
            continue;
        }

        /*
         * This is the filter that implements the "learn only your own links"
         * rule. If neither end of the link is us, the line describes a part of
         * the network we are not allowed to know about yet, so we drop it.
         * We will learn about it later from a flooded LSA.
         */
        if (a != self_id && b != self_id) {
            continue;
        }

        /* The neighbor is whichever end is not us. */
        int neighbor_id = (a == self_id) ? b : a;

        if (already_have_neighbor(out, count, neighbor_id)) {
            fprintf(stderr, "topology: %s:%d: duplicate link to router %d, skipping\n",
                    path, lineno, neighbor_id);
            continue;
        }

        if (count >= max_out) {
            fprintf(stderr, "topology: %s:%d: more than %d neighbors for router %d\n",
                    path, lineno, max_out, self_id);
            fclose(f);
            return -1;
        }

        out[count].id = neighbor_id;
        out[count].cost = cost;
        count++;
    }

    /* ferror() distinguishes "reached end of file" from "read failed". */
    if (ferror(f)) {
        perror(path);
        fclose(f);
        return -1;
    }

    if (fclose(f) != 0) {
        perror(path);
        return -1;
    }

    return count;
}
