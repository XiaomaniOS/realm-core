#include <string>
#include <cstdlib> // size_t
#ifndef _MSC_VER
#include <stdint.h>
#else
#include <win32/stdint.h>
#endif

#include <iostream>
#include <tightdb/assert.hpp>
#include <tightdb/utilities.hpp>

#ifdef _MSC_VER
    #include <intrin.h>
#endif

namespace tightdb {

char sse_support = -1;

void cpuid_init()
{
#ifdef TIGHTDB_COMPILER_SSE
    int cret;
#ifdef _MSC_VER
    int CPUInfo[4];
    __cpuid(CPUInfo, 1);
    cret = CPUInfo[2];
#else
    int a = 1;
    __asm ( "mov %1, %%eax; "            // a into eax
          "cpuid;"
          "mov %%ecx, %0;"             // ecx into b
          :"=r"(cret)                     // output
          :"r"(a)                      // input
          :"%eax","%ebx","%ecx","%edx" // clobbered register
         );
#endif

// Byte is atomic. Race can/will occur but that's fine
    if(cret & 0x100000) // test for 4.2
        sse_support = 1;
    else if(cret & 0x1) // Test for 3
        sse_support = 0;
    else
        sse_support = -2;
#endif
}

size_t to_ref(int64_t v)
{
#ifdef TIGHTDB_DEBUG
    uint64_t m = size_t(-1);
    TIGHTDB_ASSERT(uint64_t(v) <= m);
    // FIXME: This misbehaves for negative v when size_t is 64-bits.
    // FIXME: This misbehaves on architectures that do not use 2's complement represenation of negative numbers.
    // FIXME: Should probably be TIGHTDB_ASSERT(0 <= v && uint64_t(v) <= numeric_limits<size_t>::max());
    // FIXME: Must also check that v is divisible by 8 (64-bit aligned).
#endif
    return size_t(v);
}

// Safe cast from 64 to 32 bits on 32 bit architecture. Differs from to_ref() by not testing alignment and REF-bitflag.
size_t to_size_t(int64_t v)
{
#ifdef TIGHTDB_DEBUG
    uint64_t m = size_t(-1);
    TIGHTDB_ASSERT(uint64_t(v) <= m);
    // FIXME: This misbehaves for negative v when size_t is 64-bits.
    // FIXME: This misbehaves on architectures that do not use 2's complement represenation of negative numbers.
    // FIXME: Should probably be TIGHTDB_ASSERT(0 <= v && uint64_t(v) <= numeric_limits<size_t>::max());
#endif
    return size_t(v);
}


void* round_up(void* p, size_t align)
{
    size_t r = ((size_t)p % align == 0 ? 0 : align - (size_t)p % align);
    return (char *)p + r;
}

void* round_down(void* p, size_t align)
{
    size_t r = (size_t)p;
    return (void *)(r & (~(align - 1)));
}

size_t round_up(size_t p, size_t align)
{
    size_t r = ((size_t)p % align == 0 ? 0 : align - (size_t)p % align);
    return p + r;
}

size_t round_down(size_t p, size_t align)
{
    size_t r = (size_t)p;
    return r & (~(align - 1));
}


void checksum_init(checksum_t* t)
{
    t->remainder = 0;
    t->remainder_len = 0;
    t->b_val = 0x794e80091e8f2bc7ULL;
    t->a_val = 0xc20f9a8b761b7e4cULL;
    t->result = 0;
}

unsigned long long checksum(unsigned char* data, size_t len)
{
    checksum_t t;
    checksum_init(&t);
    checksum_rolling(data, len, &t);
    return t.result;
}

void checksum_rolling(unsigned char* data, size_t len, checksum_t* t)
{
    while (t->remainder_len < 8 && len > 0)
    {
        t->remainder = t->remainder >> 8;
        t->remainder = t->remainder | (unsigned long long)*data << (7*8);
        t->remainder_len++;
        data++;
        len--;
    }

    if (t->remainder_len < 8)
    {
        t->result = t->a_val + t->b_val;
        return;
    }

    t->a_val += t->remainder * t->b_val;
    t->b_val++;
    t->remainder_len = 0;
    t->remainder = 0;

    while (len >= 8)
    {
#ifdef TIGHTDB_X86_OR_X64
        t->a_val += (*(unsigned long long *)data) * t->b_val;
#else
        unsigned long long l = 0;
        for (unsigned int i = 0; i < 8; i++)
        {
            l = l >> 8;
            l = l | (unsigned long long)*(data + i) << (7*8);
        }
        t->a_val += l * t->b_val;
#endif
        t->b_val++;
        len -= 8;
        data += 8;
    }

    while (len > 0)
    {
        t->remainder = t->remainder >> 8;
        t->remainder = t->remainder | (unsigned long long)*data << (7*8);
        t->remainder_len++;
        data++;
        len--;
    }

    t->result = t->a_val + t->b_val;
    return;
}

}
