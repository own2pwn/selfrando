
cmake_policy(SET CMP0063 NEW)

set(SR_RNG "urandom" CACHE STRING
    "Random number generator to use ('rand_r' or 'urandom')")

set(ALLOWED_RNGS rand_r urandom)
set_property(CACHE SR_RNG PROPERTY STRINGS ${ALLOWED_RNGS})
list(FIND ALLOWED_RNGS ${SR_RNG} rng_idx)
if(${rng_idx} LESS 0)
    message(FATAL_ERROR "Unknown RNG '${SR_RNG}'!")
endif()
string(TOUPPER ${SR_RNG} upper_rng)
add_definitions(-DRANDOLIB_RNG_IS_${upper_rng}=1)

if (SR_BUILD_MODULE_RANDOLIB)
    # Make stack non-executable in assembly files
    add_compile_options(-Wa,--noexecstack)

    # We don't want to depend on memset/memcpy/the rest
    if ("${SR_ARCH}" STREQUAL "x86" OR "${SR_ARCH}" STREQUAL "x86_64")
        add_compile_options(-minline-all-stringops)
    endif()

    add_compile_options(-fno-exceptions)

    # Since we're trying to avoid depending on glibc,
    # we have to disable the stack protector
    add_compile_options(-fno-stack-protector)

    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,noexecstack")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_ASM_VISIBILITY_PRESET hidden)

    if("${SR_ARCH}" STREQUAL "x86")
        add_compile_options(-m32)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m32")
    endif()

    # Create the entry libraries
    set(entry_files
        ${SR_ARCH}/EntryPointWrapper.S
        ${SR_ARCH}/Textramp.S
        EntryPoint.c)
    if(ANDROID_NDK)
        list(APPEND entry_files ${SR_ARCH}/AndroidInit.c)
    endif()
    if("${SR_ARCH}" STREQUAL "arm")
        list(APPEND entry_files arm/LinkerStubs.S)
    endif()
    add_library(randoentry_exec STATIC ${entry_files} EntryPointExec.c)
    add_library(randoentry_so   STATIC ${entry_files} EntryPointSO.c)

    # Build the trapheader/footer objects
    add_library(trapheader        STATIC TrapHeader.S)
    add_library(trapheader_page   STATIC TrapHeaderPage.S)
    add_library(trapfooter        STATIC TrapFooter.S)
    add_library(trapfooter_page   STATIC TrapFooterPage.S)
    add_library(trapfooter_nopage STATIC TrapFooterNoPage.S)

    set(static_libs
        randoentry_exec
        randoentry_so
        trapheader
        trapheader_page
        trapfooter
        trapfooter_page
        trapfooter_nopage
        )
    foreach(static_lib ${static_libs})
        add_custom_command(TARGET ${static_lib}
            POST_BUILD
            COMMAND objcopy --rename-section .text.__x86.get_pc_thunk.bx=.selfrando.text.__x86.get_pc_thunk.bx $<TARGET_FILE:${static_lib}>
            COMMAND objcopy --rename-section .text.Linux_EntryPoint__x86.get_pc_thunk.bx=.text.__x86.get_pc_thunk.bx $<TARGET_FILE:${static_lib}>
            COMMAND objcopy --prefix-symbols=_TRaP_ $<TARGET_FILE:${static_lib}>
            COMMAND objcopy --redefine-sym=_TRaP__GLOBAL_OFFSET_TABLE_=_GLOBAL_OFFSET_TABLE_ $<TARGET_FILE:${static_lib}>
            COMMAND objcopy --redefine-sym=_TRaP__DYNAMIC=_DYNAMIC $<TARGET_FILE:${static_lib}>
            COMMAND objcopy --redefine-sym=_TRaP_dl_iterate_phdr=dl_iterate_phdr $<TARGET_FILE:${static_lib}>
            # On 32-bit x86, we need __x86.get_pc_thunk.bx to be defined as the
            # compiler emits it: a single-element group section
            # FIXME: it would be nice to get rid of this
            COMMAND objcopy --redefine-sym=_TRaP_Linux_EntryPoint__x86.get_pc_thunk.bx=__x86.get_pc_thunk.bx $<TARGET_FILE:${static_lib}>
            VERBATIM
            )
    endforeach()
    install(TARGETS
        ${static_libs}
        DESTINATION bin/${SR_ARCH}
        )

    add_library(selfrando_objects OBJECT
        ../RandoLib.cpp
        OSImpl.cpp
        ../util/hash_32a.c
        ../util/strtol.c
        ../util/qsort.c
        ../util/printf.cpp
        ../arch/${SR_ARCH}/Util.cpp
        ${SR_ARCH}/Relocation.cpp
        ${SR_ARCH}/Cache.cpp
        )

    # Add dependency on libelf since we import ELF headers
    if (SR_LIBELF_TARGET)
        add_dependencies(selfrando_objects ${SR_LIBELF_TARGET})
    endif()

    if("${SR_ARCH}" STREQUAL "arm")
        target_sources(selfrando_objects PRIVATE arm/UnwindStubs.c)
    endif()

    if("${SR_RNG}" STREQUAL "urandom")
        target_sources(selfrando_objects PRIVATE util/rand_linux.cpp)
    endif()

    include(bionic/CMakeLists.txt)

    # TODO: include all the Android options

    # FIXME: scons can build both a shared and static library
    # from the same source files but with different C defs
    # but that's not as simple with cmake
    if(BUILD_SHARED_LIBS)
        target_compile_definitions(selfrando_objects PRIVATE -DRANDOLIB_IS_SHARED=1)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libstdc++ -nostdlib")
        add_library(selfrando SHARED $<TARGET_OBJECTS:selfrando_objects>)
        target_link_libraries(selfrando PRIVATE dl)
        install(TARGETS selfrando DESTINATION bin/${SR_ARCH})
    else()
        add_library(selfrando_noprefix STATIC $<TARGET_OBJECTS:selfrando_objects>)

        # Build libselfrando.a with .selfrando prefix prepended to all sections
        add_custom_target(libselfrando.a ALL
            COMMAND objcopy --prefix-alloc-sections=.selfrando libselfrando_noprefix.a libselfrando.a
            COMMAND objcopy --rename-section .selfrando.eh_frame=.eh_frame libselfrando.a
            COMMAND objcopy --prefix-symbols=_TRaP_ libselfrando.a
            COMMAND objcopy --redefine-sym=_TRaP__GLOBAL_OFFSET_TABLE_=_GLOBAL_OFFSET_TABLE_ libselfrando.a
            COMMAND objcopy --redefine-sym=_TRaP_environ=environ libselfrando.a
            COMMAND objcopy --redefine-sym=_TRaP_dl_iterate_phdr=dl_iterate_phdr libselfrando.a
            VERBATIM
            )
        add_dependencies(libselfrando.a selfrando_noprefix)
        install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/libselfrando.a
            DESTINATION bin/${SR_ARCH})

        # Build selfrando_txtrp.o with all the code in .txtrp
        # FIXME: is this list correct?
        set(ALL_LDEMULS elf_i386 elf_x86_64 armelf aarch64linux)
        list(FIND ALL_ARCHES ${SR_ARCH} arch_idx)
        list(GET ALL_LDEMULS ${arch_idx} arch_ldemul)
        add_custom_target(selfrando_txtrp.o ALL
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/rename_sections_txtrp.sh
                libselfrando_noprefix.a
                selfrando_txtrp.o
            COMMAND objcopy --prefix-symbols=_TRaP_ selfrando_txtrp.o
            COMMAND objcopy --redefine-sym=_TRaP__GLOBAL_OFFSET_TABLE_=_GLOBAL_OFFSET_TABLE_ selfrando_txtrp.o
            COMMAND objcopy --redefine-sym=_TRaP_environ=environ selfrando_txtrp.o
            COMMAND objcopy --redefine-sym=_TRaP_dl_iterate_phdr=dl_iterate_phdr selfrando_txtrp.o
            VERBATIM
            )
        add_dependencies(selfrando_txtrp.o selfrando_noprefix)
        # FIXME: also install into arch-specific subdirectory
        install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/selfrando_txtrp.o
            DESTINATION bin)
    endif()

endif()
