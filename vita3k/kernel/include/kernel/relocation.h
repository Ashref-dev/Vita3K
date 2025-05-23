// Vita3K emulator project
// Copyright (C) 2025 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <mem/util.h>

#include <map>

struct MemState;

struct SegmentInfoForReloc {
    Address addr; // segment address in guest memory
    Address p_vaddr; // segment virtual address in guest memory
    uint64_t size; // segment memory size
};
using SegmentInfosForReloc = std::map<uint16_t, SegmentInfoForReloc>;

/**
 * \param is_var_import True when alternate format 1 should be used (it's used for var import relocations)
 * \param explicit_symval Used only if is_var_import is true, specifies the value to be written to the relocation target
 * \return True on success, false on error
 */
bool relocate(const void *entries, uint32_t size, const SegmentInfosForReloc &segments, const MemState &mem, bool is_var_import = false, uint32_t explicit_symval = 0);
