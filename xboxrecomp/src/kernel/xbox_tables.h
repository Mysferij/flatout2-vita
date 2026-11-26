/*
 * xbox_tables.h -- the runtime's big fixed tables, allocated at start-up.
 *
 * These used to be static arrays, about 25 MB of .bss. On the PS Vita the
 * whole .bss counts against the app's memory, and that size kept the eboot
 * from loading (C1-2613-2). Each table is a pointer now, calloc'ed once by
 * xbox_tables_alloc(), which game_main() calls before anything else. A table
 * that is needed only by one renderer is allocated by that renderer's
 * install call (nv2a_gl_install).
 */
#ifndef XBOX_TABLES_H
#define XBOX_TABLES_H

#include <stddef.h>
#include <stdlib.h>

/* Allocate every kernel-side table. Call once, before the title starts. */
void xbox_tables_alloc(void);

/* Out of memory while allocating a table: report and abort. */
void xbox_table_oom(const char *what, size_t count, size_t size);

/* Zeroed storage for p[n], unless p is already allocated. */
#define XBOX_TABLE_ALLOC(p, n)                                              \
    do {                                                                    \
        if (!(p) && !((p) = calloc((size_t)(n), sizeof *(p))))              \
            xbox_table_oom(#p, (size_t)(n), sizeof *(p));                   \
    } while (0)

#endif
