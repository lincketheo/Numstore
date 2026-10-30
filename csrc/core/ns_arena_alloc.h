/// Copyright 2026 Theo Lincke
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.

/**
 * @file
 * @brief Allocators and memory allocation patterns
 *
 * Alloc.h contains specialized allocators used in numstore
 * such as:
 * - Slab Allocator  - an allocator that allocates fixed size slabs
 * - Malloc Plan     - a two phase malloc - defining the size - then one malloc
 */

#ifndef NS_CHUNK_ALLOC_H
#define NS_CHUNK_ALLOC_H

#include "core/ns_error.h"
#include "core/ns_latch.h"
#include "core/ns_stdtypes.h"
#include "core/os/ns_malloc.h"

struct arena_alloc_settings
{
  u32          max_alloc_size;
  u32          max_total_size;
  float        target_chunk_mult;
  u32          min_chunk_size;
  u32          max_chunk_size;
  u32          max_chunks;
  struct i_mem mem;
};

struct arena_alloc
{
  latch                       latch;
  struct arena_alloc_settings settings;
  struct chunk               *head;
  u32                         num_chunks;
  u32                         total_allocated;
  u32                         total_used;
};

#ifndef NDEBUG
void arena_alloc_assert (const struct arena_alloc *a);
DEFINE_DBG_ASSERT (struct arena_alloc, arena_alloc, ca, { arena_alloc_assert (ca); })
#endif

void arena_alloc_create (struct arena_alloc *dest, struct arena_alloc_settings settings);
void arena_alloc_create_default_with (struct arena_alloc *dest, struct i_mem mem);
void arena_alloc_create_default (struct arena_alloc *dest);
void *arena_malloc (struct arena_alloc *ca, u32 req, u32 size, error *e);
void arena_alloc_free_all (struct arena_alloc *ca);
void *arena_alloc_copy (struct arena_alloc *alloc, const void *ptr, u32 size, error *e);

#define ALLOC_INIT(name)   \
  struct arena_alloc name; \
  arena_alloc_create_default (&(name))

#define ALLOC_CLOSE(name) arena_alloc_free_all (&(name))

#define ALLOC_RESET(name)         \
  arena_alloc_free_all (&(name)); \
  arena_alloc_create_default (&(name))

/******************************************************************************
 * SECTION: Builder Pattern
 * ----------------------------------------------------------------------------
 * @brief Contains two allocators - a persistent allocator and a temp
 ******************************************************************************/

struct builder
{
  struct arena_alloc *persistent;
  struct arena_alloc  temp;
};

void builder_init (struct builder *b, struct arena_alloc *alloc);
void *builder_malloc_temp (struct builder *b, u32 nelem, u32 size, error *e);
void *builder_malloc_persist (struct builder *b, u32 nelem, u32 size, error *e);
void builder_free (struct builder *b);

#define BUILDER_INIT(name, alloc) \
  struct builder name;            \
  builder_init (&(name), alloc)

#define BUILDER_CLOSE(name) builder_free (&(name))

#endif
