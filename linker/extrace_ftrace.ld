/* SPDX-License-Identifier: MIT */
/*
 * extrace_ftrace.ld - function entry tracing (docs/ftrace.md), INCLUDE-d from an
 * image's linker script: the compiler's table of patch sites stays in the ELF for
 * the host and is never loaded, so it costs no target memory. Leave this file
 * out to keep the table on the device (__start/__stop___patchable_function_entries).
 * Entries of functions removed by --gc-sections go with them.
 */
SECTIONS
{
    __patchable_function_entries 0 (INFO) :
    {
        *(__patchable_function_entries)
    }
}
