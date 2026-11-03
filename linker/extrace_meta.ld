/* SPDX-License-Identifier: MIT */
/*
 * extrace_meta.ld - metadata placement, INCLUDE-d from an image's linker script
 * (DESIGN §10.5, mode INFO): kept in the ELF, never loaded, at address 0, so a
 * printf site ID is the descriptor address >> 5. The site descriptors come first.
 */
SECTIONS
{
    .extrace_meta 0 (INFO) :
    {
        KEEP(*(extrace_meta_site))
        KEEP(*(extrace_meta_ev))
        KEEP(*(extrace_meta_grp))
        KEEP(*(extrace_meta_img))
        KEEP(*(extrace_meta_str))
    }
}
