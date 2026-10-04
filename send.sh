#!/usr/bin/env bash
#
# send.sh - inject one data packet into the running network.
#
# usage: ./send.sh <from> <to> <message>
#
# The packet is handed to the router it originates from, which then forwards it
# hop by hop toward <to> using its own routing table. Watch the routers' logs
# (or logs/r<id>.log under run.sh) to see the journey.
#
# Example:
#   ./send.sh 1 3 hello world
# produces, across three separate processes:
#   [R1] forwarding DATA 1->3 via R2 (ttl 7)
#   [R2] forwarding DATA 1->3 via R3 (ttl 6)
#   [R3] delivered DATA from R1: "hello world" (ttl 6)

set -euo pipefail

# Starting time to live. Decremented at every hop, so a packet can cross at
# most this many routers before being dropped. Generous for a 16 router network.
TTL=8

BASE_PORT=5000

if [ "$#" -lt 3 ]; then
    echo "usage: $0 <from> <to> <message>" >&2
    exit 1
fi

from=$1
to=$2
shift 2

# Everything left is the message, so the payload may contain spaces.
message="$*"

# Check the IDs are numeric before using one to compute a port number, so a
# typo produces a clear error instead of an arithmetic failure.
for id in "$from" "$to"; do
    case "$id" in
        ''|*[!0-9]*)
            echo "$0: router ID \"$id\" must be a positive integer" >&2
            exit 1
            ;;
    esac
done

port=$((BASE_PORT + from))

# -u is UDP, and -w1 makes netcat give up after a second rather than waiting
# for a reply that a one-way data packet is never going to get.
printf 'DATA %s %s %s %s' "$from" "$to" "$TTL" "$message" |
    nc -u -w1 127.0.0.1 "$port"

echo "sent DATA $from -> $to (ttl $TTL) to 127.0.0.1:$port"
