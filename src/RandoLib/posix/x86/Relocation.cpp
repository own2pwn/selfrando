/*
 * This file is part of selfrando.
 * Copyright (c) 2015-2017 Immunant Inc.
 * For license information, see the LICENSE file
 * included with selfrando.
 *
 */

#include <OS.h>
#include <TrapInfo.h>

#include <elf.h>

namespace os {

// bfd converts some R_386_GOT32[X] relocations to either
// R_386_GOTOFF (which is fine for us) or to R_386_32 relocations
// (which we need to detect and handle)
static inline bool is_patched_got32(BytePointer at_ptr, bool incl_lea) {
    if (incl_lea && at_ptr[-2] == 0x8d)
        return true;                // mov   foo@GOT, ...   => lea foo@GOTOFF, ...
    return at_ptr[-2] == 0xc7 ||    // mov   foo@GOT, ...   => mov $foo, ...
           at_ptr[-2] == 0xf7 ||    // test  ..., %foo@GOT  => test ..., $foo
           at_ptr[-2] == 0x81;      // binop foo@GOT, ...   => binop $foo, ...
}

static inline bool is_patched_tls_get_addr_call(BytePointer at_ptr) {
    // TLS GD-IE or GD-LE transformation in gold:
    // replaces a call to __tls_get_addr with a
    // MOV from GS:0 to EAX
    // Format is: 65 A1 00 00 00 00 MM NN ...
    // where MM is one of 81, 2D, 03, 90, or 8D and
    // the total length is either 11 or 12 byte
    auto at_ptr32 = reinterpret_cast<uint32_t*>(at_ptr);
    if (at_ptr32[-2] == 0x0000a165 && at_ptr32[-1] == 0xe8810000)
        return true;
    if (at_ptr32[-2] == 0x0000a165 && at_ptr32[-1] == 0x83030000)
        return true;
    if (at_ptr32[-2] == 0x0000a165 && at_ptr32[-1] == 0xb68d0000)
        return true;
    if ((at_ptr32[-2] >> 8) == 0x00a165 && at_ptr32[-1] == 0x2d000000)
        return true;
    if ((at_ptr32[-2] >> 8) == 0x00a165 && at_ptr32[-1] == 0x90000000)
        return true;
    return false;
}

BytePointer Module::Relocation::get_target_ptr() const {
    // IMPORTANT: Keep TrapInfo/TrapInfoRelocs.h in sync whenever a new
    // relocation requires a symbol and/or addend.
    switch(m_type) {
    case R_386_32:
    abs32_reloc:
        return reinterpret_cast<BytePointer>(*reinterpret_cast<uint32_t*>(m_src_ptr));
    case R_386_GOT32:
    case 43: // R_386_GOT32X
        if (is_patched_got32(m_src_ptr, false))
            goto abs32_reloc;
        // Compilers may try to indirectly call __tls_get_addr
        // through the GOT, which would be encoded as an indirect
        // call with a R_386_GOT32X relocation
        if (m_type == 43 &&
            is_patched_tls_get_addr_call(m_src_ptr))
            return nullptr;
        // Fall-through
    case R_386_GOTOFF:
        return m_module.get_got_ptr() + *reinterpret_cast<ptrdiff_t*>(m_src_ptr);
    case R_386_PC32:
    case R_386_PLT32:
    case R_386_GOTPC:
        if (is_patched_tls_get_addr_call(m_src_ptr))
            return nullptr;
        // We need to use the original address as the source here (not the diversified one)
        // to keep in consistent with the original relocation entry (before shuffling)
        return m_orig_src_ptr - m_addend + *reinterpret_cast<int32_t*>(m_src_ptr);
    default:
        return nullptr;
    }
}

void Module::Relocation::set_target_ptr(BytePointer new_target) {
    switch(m_type) {
    case R_386_32:
    abs32_reloc:
        *reinterpret_cast<uint32_t*>(m_src_ptr) = reinterpret_cast<uintptr_t>(new_target);
        break;
    case R_386_GOT32:
    case 43: // R_386_GOT32X
        if (is_patched_got32(m_src_ptr, false))
            goto abs32_reloc;
        // See comment in get_target_ptr()
        if (m_type == 43 &&
            is_patched_tls_get_addr_call(m_src_ptr))
            break;
        // Fall-through
    case R_386_GOTOFF:
        *reinterpret_cast<ptrdiff_t*>(m_src_ptr) = new_target - m_module.get_got_ptr();
        break;
    case R_386_PC32:
    case R_386_PLT32:
    case R_386_GOTPC:
        if (is_patched_tls_get_addr_call(m_src_ptr))
            break;
        // FIXME: check for overflow here???
        *reinterpret_cast<int32_t*>(m_src_ptr) = static_cast<int32_t>(new_target + m_addend - m_src_ptr);
        break;
    default:
        RANDO_ASSERT(false);
        break;
    }
}

BytePointer Module::Relocation::get_got_entry() const {
    switch(m_type) {
    case R_386_GOT32:
    case 43: // R_386_GOT32X
        if (is_patched_got32(m_src_ptr, true))
            return nullptr;
        return m_module.get_got_ptr() + *reinterpret_cast<int32_t*>(m_src_ptr) - m_addend;
    default:
        return nullptr;
    }
}

Module::Relocation::Type Module::Relocation::get_pointer_reloc_type() {
    return R_386_32;
}

void Module::Relocation::fixup_export_trampoline(BytePointer *export_ptr,
                                                 const Module &module,
                                                 FunctionList *functions) {
    if (**export_ptr == 0xEB) {
        // We hit the placeholder in Textramp.S, skip over it
        *export_ptr += 2;
        return;
    }
    // Allow the first byte of the export trampoline to be 0xCC, which
    // is the opcode for the breakpoint instruction that gdb uses (INT 3)
    RANDO_ASSERT(**export_ptr == 0xE9 || **export_ptr == 0xCC);
    RANDO_ASSERT((reinterpret_cast<uintptr_t>(*export_ptr) & 1) == 0);
    Module::Relocation reloc(module, *export_ptr + 1, R_386_PC32, -4);
    functions->adjust_relocation(&reloc);
    *export_ptr += 6;
}

void Module::Relocation::fixup_entry_point(const Module &module,
                                           uintptr_t entry_point,
                                           uintptr_t target) {
    RANDO_ASSERT(*reinterpret_cast<uint8_t*>(entry_point) == 0xE9);
    Module::Relocation reloc(module, entry_point + 1, R_386_PC32, -4);
    reloc.set_target_ptr(reinterpret_cast<BytePointer>(target));
}

template<>
size_t Module::arch_reloc_type<Elf32_Rel>(const Elf32_Rel *rel) {
    auto rel_type = ELF32_R_TYPE(rel->r_info);
    if (rel_type == R_386_RELATIVE ||
        rel_type == R_386_GLOB_DAT ||
        rel_type == R_386_32) {
        return R_386_32;
    }
    return 0;
}

void Module::preprocess_arch() {
    m_linker_stubs = 0;
    build_arch_relocs<Elf32_Dyn, Elf32_Rel, DT_REL, DT_RELSZ>();
}

void Module::relocate_arch(FunctionList *functions) const {
}

} // namespace os
