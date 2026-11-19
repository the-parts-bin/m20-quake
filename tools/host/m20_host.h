// Included ahead of every source in the host build (make host, the
// Makefile: -include): the RISC-V instructions the port writes as inline
// assembly (the data cache's cbo.*, the CSRs) as assembler macros that
// expand to nothing, so that the same sources build for x86. The host's
// memory is coherent (the span emulator reads it when a command is
// written), so the cache operations have nothing to do; the CSRs are the
// trap vector and the interrupt enables, which the harness doesn't need.
// The asm statements stay compiler barriers.
#ifndef M20_HOST_H
#define M20_HOST_H

__asm__(".macro cbo.clean a\n.endm\n"
        ".macro cbo.flush a\n.endm\n"
        ".macro cbo.inval a\n.endm\n"
        ".macro cbo.zero a\n.endm\n"
        ".macro csrw a, b\n.endm\n"
        ".macro csrs a, b\n.endm\n"
        ".macro csrc a, b\n.endm\n"
        ".macro csrr a, b\n.endm\n");

#endif
