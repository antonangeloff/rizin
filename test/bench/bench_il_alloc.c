// SPDX-FileCopyrightText: 2026 <anton.angelov@protonmail.com>
// SPDX-License-Identifier: LGPL-3.0-only

#include "bench_utils.h"
#include <rz_util.h>
#include <rz_il/rz_il_alloc.h>

/**
 * \file bench_il_alloc.c
 *
 * \brief Benchmark comparing raw malloc/free against the arena allocator.
 *
 * Each run allocates BENCH_ALLOC_COUNT blocks whose sizes cycle through `sizes`.
 */

#define BENCH_ALLOC_COUNT 20000
#define BENCH_ITERATIONS 100
#define BENCH_ARENA_INITIAL_CAPACITY 4096

static const size_t sizes[] = { 16, 40, 80, 160, 200 };

static void bench_malloc_direct(const char *title, RzTable *t_out) {
	size_t size_count = RZ_ARRAY_SIZE(sizes);
	void **ptrs = malloc(sizeof(void *) * BENCH_ALLOC_COUNT);
	RZ_BENCH_RUN(title, t_out, BENCH_ITERATIONS, {
		for (size_t i = 0; i < BENCH_ALLOC_COUNT; i++) {
			ptrs[i] = malloc(sizes[i % size_count]);
		}
		for (size_t i = 0; i < BENCH_ALLOC_COUNT; i++) {
			free(ptrs[i]);
		}
	});
	free(ptrs);
}

static void bench_malloc_passthrough(const char *title, RzTable *t_out) {
	size_t size_count = RZ_ARRAY_SIZE(sizes);
	void **ptrs = malloc(sizeof(void *) * BENCH_ALLOC_COUNT);
	RZ_BENCH_RUN(title, t_out, BENCH_ITERATIONS, {
		for (size_t i = 0; i < BENCH_ALLOC_COUNT; i++) {
			ptrs[i] = il_alloc_new(il_alloc_stdlib_allocator(), sizes[i % size_count]);
		}
		for (size_t i = 0; i < BENCH_ALLOC_COUNT; i++) {
			il_alloc_free(ptrs[i]);
		}
	});
	free(ptrs);
}

static void bench_arena_mm(const char *title, RzTable *t_out) {
	size_t size_count = RZ_ARRAY_SIZE(sizes);
	void **ptrs = malloc(sizeof(void *) * BENCH_ALLOC_COUNT);
	RZ_BENCH_RUN(title, t_out, BENCH_ITERATIONS, {
		ILAlloc *arena = il_alloc_arena_new(IL_ARENA_MODE_MM, BENCH_ARENA_INITIAL_CAPACITY);
		for (size_t i = 0; i < BENCH_ALLOC_COUNT; i++) {
			ptrs[i] = il_alloc_new(arena, sizes[i % size_count]);
		}
		for (size_t i = 0; i < BENCH_ALLOC_COUNT; i++) {
			il_alloc_free(ptrs[i]);
		}
		il_alloc_arena_free(arena);
	});
	free(ptrs);
}

static void bench_arena_gc(const char *title, RzTable *t_out) {
	size_t size_count = RZ_ARRAY_SIZE(sizes);
	RZ_BENCH_RUN(title, t_out, BENCH_ITERATIONS, {
		ILAlloc *arena = il_alloc_arena_new(IL_ARENA_MODE_GC, BENCH_ARENA_INITIAL_CAPACITY);
		for (size_t i = 0; i < BENCH_ALLOC_COUNT; i++) {
			il_alloc_new(arena, sizes[i % size_count]);
		}
		il_alloc_arena_free(arena);
	});
}

int main() {
	RzTable *t = rz_table_new();
	RZ_BENCH_TABLE_INIT(t);

	char title[128];
	rz_strf(title, "malloc (direct): %" PFMT64u " allocations", (ut64)BENCH_ALLOC_COUNT);
	bench_malloc_direct(title, t);

	rz_strf(title, "malloc (passthrough): %" PFMT64u " allocations", (ut64)BENCH_ALLOC_COUNT);
	bench_malloc_passthrough(title, t);

	rz_strf(title, "arena (MM): %" PFMT64u " allocations", (ut64)BENCH_ALLOC_COUNT);
	bench_arena_mm(title, t);

	rz_strf(title, "arena (GC): %" PFMT64u " allocations", (ut64)BENCH_ALLOC_COUNT);
	bench_arena_gc(title, t);

	RZ_BENCH_TABLE_PRINT_AND_FREE(t);
	return 0;
}
