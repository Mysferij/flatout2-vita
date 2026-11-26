/*
 * xbox_tables.c -- start-up allocation of the kernel's big tables (see
 * xbox_tables.h). Each module defines its own <module>_tables_alloc().
 */
#include <stdio.h>
#include <stdlib.h>

#include "xbox_tables.h"

void guest_vmem_tables_alloc(void);
void kernel_rtl_tables_alloc(void);
void xbox_memory_layout_tables_alloc(void);
void nv2a_pb_exec_tables_alloc(void);

void xbox_tables_alloc(void)
{
    guest_vmem_tables_alloc();
    kernel_rtl_tables_alloc();
    xbox_memory_layout_tables_alloc();
    nv2a_pb_exec_tables_alloc();
}

void xbox_table_oom(const char *what, size_t count, size_t size)
{
    fprintf(stderr, "[FATAL] out of memory: %s (%lu x %lu bytes)\n",
            what, (unsigned long)count, (unsigned long)size);
    abort();
}
