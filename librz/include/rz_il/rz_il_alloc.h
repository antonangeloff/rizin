// SPDX-FileCopyrightText: 2026 <anton.angelov@protonmail.com>
// SPDX-License-Identifier: LGPL-3.0-only

#ifndef IL_ALLOC_H
#define IL_ALLOC_H

#include <rz_types.h>
#include <rz_th.h>
#include <rz_util/rz_intervaltree.h>
#include <rz_util/rz_log.h>

/**
 * \file
 * \brief Custom memory allocator with "passthrough" and "memory arena" backends.
 *
 * Passthrough allocator:
 *
 * 		A thin wrapper around `malloc()`/`free()`/`realloc()`.
 *
 * Memory arena allocator:
 *
 * 		Pre-allocates a large buffer (potentially split in chunks) and breaks it
 * 		into smaller blocks, allocated through an ever-increasing `offset` index.
 * 		The arena itself is reference-counted.
 *
 * 		Supports two modes:
 *
 * 		  - Manual Memory Management (MM): each allocated block is expected to be
 * 				freed by the user. The arena is freed when all it's references are dropped
 * 				and all it's blocks are freed (which also keep a reference to the arena).
 * 				Attempting to free a block more than once logs a warning, and doesn't
 * 				decrease references further.
 *
 * 		  - Garbage Collection (GC): blocks are not freed by the user individually, but in
 *	 			bulk when the arena is torn down. The arena is freed when it's
 *				references are dropped (blocks don't hold references to the arena in
 *				this mode). Attempting to free a GC'ed block generates a warning.
 *
 * Examples:
 *
 * 		```
 * 		// Passthrough (stdlib) allocator
 * 		void *block = il_alloc_new(il_alloc_stdlib_allocator(), size);
 * 		il_alloc_free(block);
 *		```
 *
 *		```
 * 		// Arena allocator (MM mode): blocks are freed individually
 * 		ILAlloc *arena = il_alloc_arena_new(IL_ARENA_MODE_MM, initial_capacity);
 * 		void *block = il_alloc_new(arena, size);
 * 		il_alloc_free(block);
 * 		il_alloc_arena_free(arena); // it's also valid to free the arena before the blocks
 *		```
 *
 *		```
 * 		// Arena allocator (GC mode): blocks are freed all at once
 * 		ILAlloc *arena = il_alloc_arena_new(IL_ARENA_MODE_GC, initial_capacity);
 * 		void *block = il_alloc_new(arena, size);
 * 		il_alloc_arena_free(arena); // frees `block` too
 *		```
 */

#define IL_ALLOC_ARENA_ALIGN 16 // should be multiple of 8
#define IL_ALLOC_ARENA_ALIGN_SHIFT 4 // log2(IL_ALLOC_ARENA_ALIGN) // todo: remove perhaps
#define IL_ALLOC_CHUNK_HDR_LOOKBACK_BITS 16
#define IL_ALLOC_ARENA_MAX_CHUNK_SIZE \
	(((size_t)1 << IL_ALLOC_CHUNK_HDR_LOOKBACK_BITS) << IL_ALLOC_ARENA_ALIGN_SHIFT)

typedef enum il_alloc_type_t {
	IL_ALLOC_TYPE_STDLIB,
	IL_ALLOC_TYPE_ARENA,
} ILAllocType;

typedef struct il_alloc_t {
	ILAllocType type;
} ILAlloc;

typedef struct il_arena_chunk_t {
	void* arena; ///< pointer back to the arena
	struct il_arena_chunk_t *next; ///< chain of all currently-live chunks
	ut32 ref_count; // todo: lock or atomic
	ut8 *buffer; ///< backing storage for arena-allocated nodes (part of the chunk's allocation unit)
	size_t capacity; ///< total capacity of the buffer
	size_t offset; ///< offset of the next free byte in buffer
} ILArenaChunk;

typedef enum il_arena_mode_t {
	IL_ARENA_MODE_MM, ///< manual memory management: blocks are freed individually via il_alloc_free()
	IL_ARENA_MODE_GC, ///< garbage collection: blocks are never freed individually; the whole arena is torn down when owner releases its last reference
} ILArenaMode;

typedef struct il_arena_alloc_t {
	ILAlloc base;
	ut32 ref_count;
	ILArenaMode mode;
	ILArenaChunk *first_chunk; ///< linked list of buffer chunks
	size_t initial_capacity; ///< capacity to be reserved for initial buffer chunk allocation
} ILArenaAlloc;

#define IL_ALLOC_ARENA_CHUNK_HDR_SIZE \
	(((sizeof(ILArenaChunk) + IL_ALLOC_ARENA_ALIGN - 1) / IL_ALLOC_ARENA_ALIGN) * IL_ALLOC_ARENA_ALIGN)

// alignof(uintptr_t): malloc's returned pointer is at least this aligned, and
// adding a multiple of it preserves that alignment for the payload pointer.
#define IL_ALLOC_BLOCK_HDR_SIZE (sizeof(struct { ut8 a; uintptr_t b; }) - sizeof(uintptr_t))


// Opaque header prepended to every block, right before the pointer returned
// to the caller.
typedef struct il_arena_block_hdr_t {
	struct {
		ut32 alloc_type : 1;
		ut32 freed : 1;
		ut32 gc : 1; ///< copied from the owning arena's mode at allocation time, so il_alloc_free() can skip GC blocks without chasing chunk->arena
		ut32 chunk_hdr_lookback : IL_ALLOC_CHUNK_HDR_LOOKBACK_BITS; ///< (block - chunk->buffer) >> IL_ALLOC_ARENA_ALIGN_SHIFT; chunk header size is added back separately since it's a compile-time constant
	};
} ILArenaBlockHdr;

// Allocate buffer chunk for a memory arena
static inline ILArenaChunk *il_arena_chunk_new(ILArenaAlloc *arena, size_t capacity) {
	if (capacity > IL_ALLOC_ARENA_MAX_CHUNK_SIZE) {
		RZ_LOG_WARN("il_arena_chunk_new: requested capacity %" PFMT64u " exceeds max chunk size %" PFMT64u ", truncating\n",
			(ut64)capacity, (ut64)IL_ALLOC_ARENA_MAX_CHUNK_SIZE);
		capacity = IL_ALLOC_ARENA_MAX_CHUNK_SIZE;
	}

	ILArenaChunk *chunk = malloc(IL_ALLOC_ARENA_CHUNK_HDR_SIZE + capacity);
	if (!chunk) {
		return NULL;
	}

	chunk->ref_count = 1;
	chunk->next = NULL;
	chunk->capacity = capacity;
	chunk->offset = 0;
	chunk->buffer = (ut8 *)chunk + IL_ALLOC_ARENA_CHUNK_HDR_SIZE;
	chunk->arena = arena;

	// Keep arena reference only in MM mode
	if (arena->mode == IL_ARENA_MODE_MM) {
		arena->ref_count += 1; // todo: atomic
	}

	return chunk;
}

// Allocate new memory arena
static inline ILAlloc *il_alloc_arena_new(ILArenaMode mode, size_t capacity) {
	ILArenaAlloc *arena = RZ_NEW0(ILArenaAlloc);
	if (!arena) {
		return NULL;
	}
	arena->base.type = IL_ALLOC_TYPE_ARENA;
	arena->mode = mode;
	arena->ref_count = 1;
	arena->first_chunk = NULL;
	arena->initial_capacity = capacity;
	return (ILAlloc *)arena;
}

// Add an arena reference, so the caller can share it with another owner
static inline ILAlloc *il_alloc_arena_add_ref(ILAlloc *alloc) {
	ILArenaAlloc *arena = (ILArenaAlloc *)alloc;
	arena->ref_count += 1; // todo: atomic
	return alloc;
}

// Release an arena reference
static inline void il_alloc_arena_free(ILAlloc *alloc) {
	ILArenaAlloc *arena = (ILArenaAlloc *)alloc;
	if (arena->ref_count-- != 1) { // todo: atomic
		return;
	}

	if (arena->mode == IL_ARENA_MODE_GC) {
		// Free all buffer chunks together with all allocated blocks
		ILArenaChunk *chunk = arena->first_chunk;
		while (chunk) {
			ILArenaChunk *next = chunk->next;
			free(chunk);
			chunk = next;
		}
	} else {
		// All references are dropped. This means chunks should already be released.
		rz_warn_if_fail(arena->first_chunk == NULL);
	}
	free(arena);
}

// Add a reference to a buffer chunk
static inline void il_arena_chunk_add_ref(ILArenaChunk *chunk) {
	chunk->ref_count += 1; // todo: atomic
}

// Release reference of a buffer chunk
static inline void il_arena_chunk_free(ILArenaChunk *chunk) {
	if (chunk->ref_count-- != 1) { // todo: atomic
		return;
	}

	ILArenaAlloc *arena = (ILArenaAlloc *)chunk->arena;
	if (arena->first_chunk == chunk) {
		arena->first_chunk = chunk->next;
	} else {
		ILArenaChunk *prev = arena->first_chunk;
		while (prev && prev->next != chunk) {
			prev = prev->next;
		}
		if (prev) {
			prev->next = chunk->next;
		}
	}
	free(chunk);
	il_alloc_arena_free((ILAlloc *)arena);
}

static inline ILAlloc *il_alloc_stdlib_allocator(void) {
	static const ILAlloc stdlib_allocator = { .type = IL_ALLOC_TYPE_STDLIB };
	return (ILAlloc *)&stdlib_allocator;
}

// Allocate a memory block via a custom allocator `alloc`
static inline void *il_alloc_new(ILAlloc *alloc, size_t size) {
	switch (alloc->type) {
	case IL_ALLOC_TYPE_STDLIB: {
		ILArenaBlockHdr *block = malloc(IL_ALLOC_BLOCK_HDR_SIZE + size); // todo: use RZ_NEW equivalent if needed
		if (!block) {
			return NULL;
		}
		block->alloc_type = IL_ALLOC_TYPE_STDLIB;
		return (ut8 *)block + IL_ALLOC_BLOCK_HDR_SIZE;
	}
	case IL_ALLOC_TYPE_ARENA: {
		// Align block size up to the arena alignment so next allocation is correctly aligned as well
		size_t block_size = ((IL_ALLOC_BLOCK_HDR_SIZE + size + IL_ALLOC_ARENA_ALIGN - 1) / IL_ALLOC_ARENA_ALIGN) * IL_ALLOC_ARENA_ALIGN;
		if (RZ_UNLIKELY(block_size > IL_ALLOC_ARENA_MAX_CHUNK_SIZE)) {
			RZ_LOG_WARN("il_alloc_new: requested size %" PFMT64u " exceeds max chunk size %" PFMT64u "\n",
				(ut64)block_size, (ut64)IL_ALLOC_ARENA_MAX_CHUNK_SIZE);
			return NULL;
		}
		ILArenaAlloc *arena = (ILArenaAlloc *)alloc;
		ILArenaChunk *chunk_ref = arena->first_chunk;
		ILArenaChunk *chunk = arena->first_chunk;
		if (RZ_UNLIKELY(!chunk)) {
			/* Lazy-initialize first chunk */
			chunk = il_arena_chunk_new(arena, RZ_MAX(arena->initial_capacity, block_size));
			if (!chunk) {
				return NULL;
			}
			arena->first_chunk = chunk;
		} else if (RZ_UNLIKELY(chunk->offset + block_size > chunk->capacity)) {
			/* Grow */
			size_t new_chunk_capacity = RZ_MIN(chunk->capacity * 2, IL_ALLOC_ARENA_MAX_CHUNK_SIZE);
			if (new_chunk_capacity < block_size) {
				new_chunk_capacity = block_size;
			}
			new_chunk_capacity = RZ_MAX(new_chunk_capacity, block_size);
			ILArenaChunk *new_chunk = il_arena_chunk_new(arena, new_chunk_capacity);
			if (!new_chunk) {
				return NULL;
			}
			new_chunk->next = chunk;
			arena->first_chunk = new_chunk;
			chunk = new_chunk;
		}
		ILArenaBlockHdr *block = (ILArenaBlockHdr *)(chunk->buffer + chunk->offset);
		chunk->offset += block_size;
		block->alloc_type = IL_ALLOC_TYPE_ARENA;
		block->freed = 0;
		block->gc = (arena->mode == IL_ARENA_MODE_GC);
		block->chunk_hdr_lookback = (ut32)(((ut8 *)block - chunk->buffer) >> IL_ALLOC_ARENA_ALIGN_SHIFT);
		il_arena_chunk_add_ref(chunk);
		if (chunk != chunk_ref) {
			// Drop the chunk's initial reference, since only the allocated blocks should keep one
			il_arena_chunk_free(chunk); // todo: just skip add_ref for new chunks
		}
		return (ut8 *)block + IL_ALLOC_BLOCK_HDR_SIZE;
	}
	}
	return NULL;
}

// Allocate a zero-initialized memory block via a custom allocator `alloc`
static inline void *il_alloc_new0(ILAlloc *alloc, size_t size) {
	switch (alloc->type) {
	case IL_ALLOC_TYPE_STDLIB: {
		ILArenaBlockHdr *block = calloc(1, size + IL_ALLOC_BLOCK_HDR_SIZE); //todo: use RZ_NEWS0
		if (!block) {
			return NULL;
		}
		block->alloc_type = IL_ALLOC_TYPE_STDLIB;
		return (ut8 *)block + IL_ALLOC_BLOCK_HDR_SIZE;
	}
	case IL_ALLOC_TYPE_ARENA: {
		void *p = il_alloc_new(alloc, size);
		if (p) {
			memset(p, 0, size);
		}
		return p;
	}
	}
	return NULL;
}

// TODO
static inline void *il_alloc_realloc(ILAlloc *alloc, void *ptr, size_t size) {
	switch (alloc->type) {
	case IL_ALLOC_TYPE_STDLIB: {
		ILArenaBlockHdr *block = (ILArenaBlockHdr *)((ut8 *)ptr - IL_ALLOC_BLOCK_HDR_SIZE);
		block = realloc(block, IL_ALLOC_BLOCK_HDR_SIZE + size);
		if (!block) {
			return NULL;
		}
		return (ut8 *)block + IL_ALLOC_BLOCK_HDR_SIZE;
	}
	case IL_ALLOC_TYPE_ARENA:
		// Not supported: no per-node size is tracked, so a safe copy-bound
		// can't be determined, and no RzIL op node is ever resized anyway.
		return NULL;
	}
	return NULL;
}

// Release a memory block allocated via custom allocator
static inline void il_alloc_free(void *ptr) {
	ILArenaBlockHdr *block = (ILArenaBlockHdr *)((ut8 *)ptr - IL_ALLOC_BLOCK_HDR_SIZE);

	switch (block->alloc_type) {
		case IL_ALLOC_TYPE_STDLIB: {
			free(block);
			return;
		}
		case IL_ALLOC_TYPE_ARENA: {
			if (RZ_UNLIKELY(block->gc)) {
				// Blocks are not supposed to be freed individually
				RZ_LOG_WARN("il_alloc_free: attempted free of a GC'd arena block %p\n", ptr);
				return;
			}
			if (RZ_UNLIKELY(block->freed)) {
				RZ_LOG_WARN("il_alloc_free: double free of arena block %p\n", ptr);
				return;
			}
			block->freed = 1;
			ILArenaChunk *chunk = (ILArenaChunk *)((ut8 *)block - IL_ALLOC_ARENA_CHUNK_HDR_SIZE - ((size_t)block->chunk_hdr_lookback << IL_ALLOC_ARENA_ALIGN_SHIFT));
			il_arena_chunk_free(chunk);
			return;
		}
	}
}

#endif // IL_ALLOC_H
