# Ultrium OS v0.0.7 — Memory groundwork

This milestone introduces an **early page-backed kernel heap**, plus a virtual-address helper for Limine's existing higher-half direct map (HHDM).

## Implemented
- `vmm_init(offset)`, `vmm_direct_map(physical)`: convert a usable physical page into its already-mapped kernel virtual address.
- `kmalloc(size)`, `kfree(pointer)`: allocate and release a single physical 4 KiB page per request.
- Basic allocation tracking with 256 statically allocated metadata slots; zero-initialize returned pages.
- A boot-time heap self-test and a heap allocation count in `ultrium system memory`.

## Explicit limits
- Requests must be between 1 and 4096 bytes. Each request consumes an entire page even if smaller.
- Up to 256 simultaneous heap allocations. No block splitting, coalescing, multipage allocation, thread-safety, or reclamation of unused metadata.
- HHDM translation **uses preexisting mappings** created by Limine. No general page-table edit or custom virtual address mapping is implemented yet.
- Designed for the single-core bootstrap kernel. `kfree` silently ignores unknown pointers; no guard pages.
- Kernel heap self-tests should report PASSED before adding dependent subsystems.

## Next stage
Implement owned page-table mapping/unmapping and a scalable, aligned, variable-sized heap; add exception handling before relying on dynamic virtual mappings, then use the heap for filesystem structures.
