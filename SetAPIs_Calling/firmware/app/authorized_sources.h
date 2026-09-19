#ifndef G100_AUTHORIZED_SOURCES_H
#define G100_AUTHORIZED_SOURCES_H

/*
 * Inbound API/discovery allow-list.
 *
 * Add one row for every controller that may call this board, then rebuild and
 * flash. Keep the final row without a trailing backslash.
 *
 * Format: G100_SOURCE(a, b, c, d, "short-label")
 */
#define G100_AUTHORIZED_SOURCE_LIST \
    G100_SOURCE(192, 168,   1,   2, "initial-source-02") \
    G100_SOURCE(192, 168,   1,   3, "initial-source-03") \
    G100_SOURCE(192, 168,   1,   4, "initial-source-04") \
    G100_SOURCE(192, 168,   1,   5, "initial-source-05") \
    G100_SOURCE(192, 168,   1,   6, "initial-source-06") \
    G100_SOURCE(192, 168,   1,   7, "initial-source-07") \
    G100_SOURCE(192, 168,   1,   8, "initial-source-08") \
    G100_SOURCE(192, 168,   1,   9, "initial-source-09") \
    G100_SOURCE(192, 168,   1,  10, "initial-source-10") \
    G100_SOURCE(192, 168,   2,   1, "this-laptop-direct") \
    G100_SOURCE(192, 168, 101, 120, "this-laptop-pineapple")

#endif
