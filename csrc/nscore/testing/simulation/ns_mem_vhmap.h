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
 * @brief Mem variable hash map
 *
 * An in memory variable hash map - used mostly for testing
 * replicating get delete insert semantics in memory
 */

#ifndef MEM_VHMAP_H
#define MEM_VHMAP_H

#include "core/ns_error.h"
#include "core/ns_ext_array.h"
#include "core/ns_htable.h"
#include "core/ns_slab_alloc.h"
#include "nscore/variables/ns_variables.h"

struct mem_vhmap
{
  struct i_mem      mem;
  struct htable    *vhasht; // Hash table of variables
  struct slab_alloc alloc;  // Allocator for variable frames
};

struct var_with_data
{
  struct variable  var;
  struct ext_array data;
};

// Lifecycle
struct mem_vhmap *mem_vhmap_create (struct i_mem mem, error *e);
void mem_vhmap_free (struct mem_vhmap *db);
struct mem_vhmap *mem_vhmap_clone (struct i_mem mem, const struct mem_vhmap *src, error *e);

// Create Get Remove
struct var_with_data *mem_vhmap_add (struct mem_vhmap *db, struct variable *var, error *e);
struct var_with_data *mem_vhmap_get (struct mem_vhmap *db, struct string name);
void mem_vhmap_remove (struct mem_vhmap *db, struct string name);

// Utilities
struct var_with_data *mem_vhmap_random (struct mem_vhmap *db);

#endif // MEM_VHMAP_H
