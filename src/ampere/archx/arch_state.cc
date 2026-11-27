//
// Copyright (c) 2019 Ampere Computing. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//

#include "ampere/archx/arch_state.h"
#include "ampere/sc/report.h"
#include <inttypes.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <boost/smart_ptr/make_local_shared.hpp>
#include <fmt/format.h>

namespace ampere {
namespace archx {

namespace {

// Call `store(address of the word, the bytes in their place in the word, mask of those bytes)` for each word that
// a block covers, in whole or in part
template<typename Store>
void for_each_word_of_block(PhysAddr addr, const uint8_t* data, size_t size, Store&& store)
{
    while (size) {
        // The largest power of two of bytes, up to a word, that `addr` is aligned to and that are left to write,
        // so that what is written never crosses a word
        size_t written = 8;
        if (addr.extract_range(0, 0) || size < 2)
            written = 1;
        else if (addr.extract_range(1, 0) || size < 4)
            written = 2;
        else if (addr.extract_range(2, 0) || size < 8)
            written = 4;

        uint64_t value = 0;
        std::memcpy(&value, data, written);

        const size_t shift = addr.extract_range(2, 0) * 8;
        const uint64_t mask = written == 8 ? UINT64_MAX : ((uint64_t{1} << (written * 8)) - 1) << shift;
        store(addr.value() & 0xfffffffffffffff8, value << shift, mask);

        size -= written;
        data += written;
        addr += written;
    }
}

} // namespace

// A memory image is millions of words, far more than a simulation goes on to write, and it never changes once it
// is loaded. Kept in a map like the one for the words written since, it takes minutes to load and makes every
// read and write walk a deep tree. So it is kept apart, in pages of words.
class ArchState::MemoryImage
{
public:
    uint64_t read(uint64_t aligned) const
    {
        const auto page = pages.find(aligned / PAGE_BYTES);
        return page == pages.end() ? 0 : page->second[(aligned % PAGE_BYTES) / 8];
    }

    void write(uint64_t aligned, uint64_t value) { pages[aligned / PAGE_BYTES][(aligned % PAGE_BYTES) / 8] = value; }

    void write(PhysAddr addr, const uint8_t* data, size_t size)
    {
        // Consecutive words are nearly always in the same page, so look the page up only when it changes
        Page* page = nullptr;
        uint64_t page_number = 0;

        for_each_word_of_block(addr, data, size, [&](uint64_t aligned, uint64_t bytes, uint64_t mask) {
            if (!page || aligned / PAGE_BYTES != page_number) {
                page_number = aligned / PAGE_BYTES;
                page = &pages[page_number];
            }
            auto& word = (*page)[(aligned % PAGE_BYTES) / 8];
            word = (word & ~mask) | bytes;
        });
    }

    // Call `visit(address, value)` for every word of every page that has been written to
    template<typename Visit>
    void for_each_word(Visit&& visit) const
    {
        for (const auto& [number, page] : pages)
            for (size_t i = 0; i < page.size(); ++i)
                visit(number * PAGE_BYTES + i * 8, page[i]);
    }

private:
    static constexpr uint64_t PAGE_BYTES = 4096;
    using Page = std::array<uint64_t, PAGE_BYTES / 8>;

    std::unordered_map<uint64_t, Page> pages; // a page that is first written to starts as zeros
};

uint64_t ArchState::read_word(uint64_t aligned) const
{
    if (const auto* value = memory.find(aligned))
        return *value;
    return image ? image->read(aligned) : 0;
}

boost::local_shared_ptr<ArchState::MemoryImage> ArchState::image_to_load_into() const
{
    auto loading = image ? boost::make_local_shared<MemoryImage>(*image) : boost::make_local_shared<MemoryImage>();

    // What is loaded takes the place of what has been written, as it did when both were in one map
    for (const auto& [address, value] : memory)
        loading->write(address, value);

    return loading;
}

ArchState ArchState::raw_write(PhysAddr addr, uint64_t value, uint64_t mask = UINT64_MAX) const
{
    ArchState state = *this;

    uint64_t aligned = addr.value() & 0xfffffffffffffff8;
    uint64_t prev = raw_read(aligned);
    size_t shift = addr.extract_range(2, 0) * 8;
    mask = mask << shift;
    state.memory = std::move(state.memory).set(aligned, (prev & ~mask) | ((uint64_t)value << shift));

    return state;
}

ArchState ArchState::write(PhysAddr addr, uint8_t value) const
{
    return raw_write(addr, uint64_t(value), uint64_t(UINT8_MAX));
}

ArchState ArchState::write(PhysAddr addr, uint16_t value) const
{
    assert(!addr.extract_range(0, 0));
    return raw_write(addr, uint64_t(value), uint64_t(UINT16_MAX));
}

ArchState ArchState::write(PhysAddr addr, uint32_t value) const
{
    assert(!addr.extract_range(1, 0));
    return raw_write(addr, uint64_t(value), uint64_t(UINT32_MAX));
}

ArchState ArchState::write(PhysAddr addr, uint64_t value) const
{
    assert(!addr.extract_range(2, 0));

    ArchState state = *this;
    state.memory = std::move(state.memory).set(addr.value(), value);
    return state;
}

ArchState ArchState::write(PhysAddr addr, const uint8_t* data, size_t size) const
{
    ArchState state = *this;
    state.write_block(addr, data, size);
    return state;
}

void ArchState::write_block(PhysAddr addr, const uint8_t* data, size_t size)
{
    // Writing each word through write(), which returns a new state, copies the part of the map that the word is
    // in every time, because the state written to still refers to it. Here the map is this state's own: set()
    // copies a part of it that another state shares the first time it changes it, and after that changes it in
    // place.
    for_each_word_of_block(addr, data, size, [this](uint64_t aligned, uint64_t bytes, uint64_t mask) {
        const uint64_t value = mask == UINT64_MAX ? bytes : (read_word(aligned) & ~mask) | bytes;
        memory = std::move(memory).set(aligned, value);
    });
}

ArchState ArchState::load_memory(std::istream& is) const
{
    auto loading = image_to_load_into();
    std::vector<std::uint8_t> data;

    // Stop at the end of the file, or at a record that is cut short, rather than load a record that was not read
    std::uint64_t address;
    std::uint32_t size;
    while (is.read((char*)&address, sizeof(address)) && is.read((char*)&size, sizeof(size))) {
        data.resize(size);
        if (!is.read((char*)data.data(), size))
            break;

        loading->write(PhysAddr{address}, data.data(), size);
    }

    ArchState state = *this;
    state.image = std::move(loading);
    state.memory = memory_map{};
    return state;
}

ArchState ArchState::load_elf(ELFIO::elfio& reader) const
{
    const uint64_t one = 1;
    bool host_little_endian = (((uint8_t*)&one)[0] == 1);
    bool elf_little_endian = (reader.get_encoding() == ELFDATA2LSB);

    if (host_little_endian != elf_little_endian)
        throw std::runtime_error("Host and ELF byte order do not match");

    auto loading = image_to_load_into();

    // What a segment has in memory: its bytes from the file, and then zeros for what the file does not hold of it,
    // which is the end of a segment that ends in a .bss and all of one that is nothing else
    std::vector<uint8_t> contents;

    for (int i = 0; i < reader.segments.size(); ++i) {
        const ELFIO::segment* seg = reader.segments[i];
        if (seg->get_type() != PT_LOAD)
            continue;

        const size_t memory_size = seg->get_memory_size();
        const size_t file_size = std::min<size_t>(seg->get_file_size(), memory_size);

        // ELFIO reads nothing for a segment that the file is too short to hold
        if (file_size && !seg->get_data())
            throw std::runtime_error(fmt::format("ELF segment {} is larger than the file", i));

        contents.assign(memory_size, 0);
        std::copy_n(seg->get_data(), file_size, contents.begin());
        loading->write(PhysAddr{seg->get_physical_address()}, contents.data(), contents.size());
    }

    ArchState state = *this;
    state.image = std::move(loading);
    state.memory = memory_map{};
    return state;
}

void ArchState::dump_memory() const
{
    // The image's pages are dumped whole, so words of them that were never loaded show as zero
    if (image) {
        image->for_each_word([this](uint64_t address, uint64_t value) {
            if (!memory.find(address))
                fmt::print("0x{:016x}: 0x{:016x}\n", address, value);
        });
    }
    for (auto& entry : memory)
        fmt::print("0x{:016x}: 0x{:016x}\n", entry.first, entry.second);
}

uint64_t ArchState::read(PhysAddr addr, size_t size) const
{
    return raw_read(addr.value(), size);
}

uint64_t ArchState::raw_read(uint64_t addr, size_t size) const
{
    if (size > 8)
        AMPERE_ERROR("Invalid read size = {} for ArchState::raw_read(uint64_t addr, size_t size). Max read size is 8.",
                     size);

    uint64_t val = 0;
    size_t read_so_far = 0;

    while (size) {
        size_t read = 0;
        uint64_t aligned = addr & (~(uint64_t)0x7);
        Bitfield aligned_8B_val = Bitfield(read_word(aligned));
        uint64_t distance_to_boundary = aligned + 8 - addr;

        unsigned lo = (addr - aligned) * 8;
        read = std::min(size, distance_to_boundary);
        unsigned hi = lo + read * 8 - 1;

        val += aligned_8B_val.extract_range(hi, lo) << read_so_far;

        read_so_far += read * 8;
        size -= read;
        addr += read;
    }

    return val;
}

uint64_t ArchState::read(LReg regno) const
{
    return gp_registers[regno];
}

ArchState ArchState::write(LReg regno, uint64_t value) const
{
    ArchState state = *this;
    state.gp_registers = std::move(state.gp_registers).set(regno, value);
    return state;
}

std::array<uint64_t, 2> ArchState::read_vfp(LReg regno) const
{
    return vfp_registers[regno];
}

ArchState ArchState::write_vfp(LReg regno, std::array<uint64_t, 2> value) const
{
    ArchState state = *this;
    state.vfp_registers = std::move(state.vfp_registers).set(regno, value);
    return state;
}

ArchState ArchState::write_vfp(LReg regno, __uint128_t value) const
{
    std::array<std::uint64_t, 2> vecval{static_cast<std::uint64_t>(value), static_cast<std::uint64_t>(value >> 64)};
    return write_vfp(regno, vecval);
}

pred_reg_t ArchState::read_pred(LReg regno) const
{
    return pred_registers[regno];
}

ArchState ArchState::write_pred(LReg regno, pred_reg_t value) const
{
    ArchState state = *this;
    state.pred_registers = std::move(state.pred_registers).set(regno, value);
    return state;
}

ArchState ArchState::write(SysReg sysreg, uint64_t value) const
{
    ArchState state = *this;
    state.sysregs = std::move(state.sysregs).set(sysreg.addr(), value);
    return state;
}

uint64_t ArchState::read(SysReg sysreg) const
{
    return sysregs[sysreg.addr()];
}

EL ArchState::exception_level() const
{
    return el;
}

ArchState ArchState::set_exception_level(EL e) const
{
    ArchState state = *this;
    state.el = e;
    return state;
}

} // namespace archx
} // namespace ampere

/*
 * vim: sw=4:ts=4:et:si:cindent:cino=\:0,(0,g0,t0:
 */
