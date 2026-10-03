/*
 * lsdb.c - storing LSAs, deciding what to flood, and aging stale entries.
 *
 * See lsdb.h for what each function promises. The interesting logic is all in
 * lsdb_install() (the flooding decision) and lsdb_link_cost() (the two-way
 * check).
 */
#include <stdio.h>
#include <string.h>

#include "lsdb.h"

/*
 * Mark a router ID as one we have heard of. Guarded against bad IDs so a
 * malformed LSA that slipped through cannot write outside the array.
 */
static void mark_known(struct lsdb *db, int id)
{
    if (id >= 1 && id <= MAX_ROUTER_ID) {
        db->known[id] = 1;
    }
}

/*
 * Cost that this LSA advertises for a link to router id, or -1 if the LSA does
 * not mention id at all.
 */
static int listed_cost(const struct lsa *l, int id)
{
    for (int i = 0; i < l->count; i++) {
        if (l->links[i].id == id) {
            return l->links[i].cost;
        }
    }
    return -1;
}

void lsdb_init(struct lsdb *db, int self_id)
{
    memset(db, 0, sizeof(*db));
    db->self_id = self_id;

    /* We always know about ourselves, even before we originate anything. */
    mark_known(db, self_id);
}

int lsdb_install(struct lsdb *db, int origin, unsigned int seq,
                 const struct neighbor_link *links, int count, long long now_ms)
{
    struct lsa *slot;

    /* Defensive: callers validate, but this function is the gate to the
     * database, so it refuses to index out of bounds under any circumstances. */
    if (origin < 1 || origin > MAX_ROUTER_ID) {
        return 0;
    }
    if (count < 0 || count > MAX_NEIGHBORS) {
        return 0;
    }

    slot = &db->entries[origin];

    /*
     * The flooding decision. We only accept strictly newer information:
     *   - no LSA from this origin yet  -> accept
     *   - seq greater than what we hold -> accept
     *   - anything else (same or older) -> ignore silently
     *
     * The silent ignore is the loop breaker. In a network with a cycle (1-2-3
     * here) a flooded LSA arrives at a router twice. The first copy is news
     * and gets forwarded; the second has the same sequence number, so it stops
     * dead and the flood terminates.
     */
    if (slot->valid && seq <= slot->seq) {
        return 0;
    }

    slot->valid = 1;
    slot->origin = origin;
    slot->seq = seq;
    slot->count = count;
    slot->recv_ms = now_ms;

    for (int i = 0; i < count; i++) {
        slot->links[i] = links[i];
        mark_known(db, links[i].id);
    }
    mark_known(db, origin);

    printf("[R%d] new LSA from R%d seq %u\n", db->self_id, origin, seq);

    return 1;
}

int lsdb_age_out(struct lsdb *db, long long now_ms, long long max_age_ms)
{
    int removed = 0;

    for (int id = 1; id <= MAX_ROUTER_ID; id++) {
        struct lsa *slot = &db->entries[id];

        if (!slot->valid) {
            continue;
        }

        /* Never expire our own LSA: we are its authority and we refresh it on
         * a timer, so an expired copy of it would be meaningless. */
        if (id == db->self_id) {
            continue;
        }

        if (now_ms - slot->recv_ms > max_age_ms) {
            printf("[R%d] LSA from R%d aged out\n", db->self_id, id);
            slot->valid = 0;
            removed++;
        }
    }

    return removed;
}

long long lsdb_next_expiry_ms(const struct lsdb *db, long long max_age_ms)
{
    long long earliest = -1;

    for (int id = 1; id <= MAX_ROUTER_ID; id++) {
        const struct lsa *slot = &db->entries[id];
        long long when;

        if (!slot->valid || id == db->self_id) {
            continue;
        }

        /* +1 ms because lsdb_age_out() uses a strict "older than" comparison;
         * waking one millisecond later guarantees the test actually passes. */
        when = slot->recv_ms + max_age_ms + 1;

        if (earliest < 0 || when < earliest) {
            earliest = when;
        }
    }

    return earliest;
}

int lsdb_link_cost(const struct lsdb *db, int a, int b)
{
    int cost_a, cost_b;

    if (a < 1 || a > MAX_ROUTER_ID || b < 1 || b > MAX_ROUTER_ID || a == b) {
        return -1;
    }

    /* Both ends must have a current LSA for us to judge the link at all. */
    if (!db->entries[a].valid || !db->entries[b].valid) {
        return -1;
    }

    cost_a = listed_cost(&db->entries[a], b);
    cost_b = listed_cost(&db->entries[b], a);

    /* The two-way check: both ends must claim the other. */
    if (cost_a < 0 || cost_b < 0) {
        return -1;
    }

    /* The spec says to use a's advertised cost. In this simulator both ends
     * read the same cost from the same topology file, so they agree. */
    return cost_a;
}

int lsdb_is_known(const struct lsdb *db, int id)
{
    if (id < 1 || id > MAX_ROUTER_ID) {
        return 0;
    }
    return db->known[id];
}
