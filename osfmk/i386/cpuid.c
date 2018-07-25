/*
 * Copyright (c) 2000-2012 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */
#include <vm/vm_page.h>
#include <pexpert/pexpert.h>

#include <i386/cpuid.h>

static	boolean_t	cpuid_dbg
#if DEBUG
				  = TRUE;
#else
				  = FALSE;
#endif
#define DBG(x...)			\
	do {				\
		if (cpuid_dbg)		\
			kprintf(x);	\
	} while (0)			\

#define min(a,b) ((a) < (b) ? (a) : (b))
#define quad(hi,lo)	(((uint64_t)(hi)) << 32 | (lo))

/* Only for 32bit values */
#define bit32(n)		(1U << (n))
#define bitmask32(h,l)		((bit32(h)|(bit32(h)-1)) & ~(bit32(l)-1))
#define bitfield32(x,h,l)	((((x) & bitmask32(h,l)) >> l))

boolean_t ForceAmdCpu = FALSE;

/* For AMD CPU's */
boolean_t IsAmdCPU(void) {
    if (ForceAmdCpu)
        return TRUE;
    
    uint32_t ourcpuid[4];
    do_cpuid(0, ourcpuid);
    if (ourcpuid[ebx] == 0x68747541 &&
        ourcpuid[ecx] == 0x444D4163 &&
        ourcpuid[edx] == 0x69746E65)
        return TRUE;
    
    return FALSE;
};

/* For Intel CPU's */
boolean_t IsIntelCPU(void) {
    uint32_t ourcpuid[4];
    do_cpuid(0, ourcpuid);
    if (ourcpuid[ebx] == 0x756E6547 &&
        ourcpuid[ecx] == 0x6C65746E &&
        ourcpuid[edx] == 0x49656E69)
        return TRUE;
    
    if (!IsAmdCPU())
        return TRUE;
    
    return FALSE;
}

uint32_t
extractBitField(uint32_t inField, uint32_t width, uint32_t offset)
{
    uint32_t bitMask;
    uint32_t outField;
    
    if ((offset+width) == 32)
    {
        bitMask = (0xFFFFFFFF<<offset);
    }
    else
    {
        bitMask = (0xFFFFFFFF<<offset) ^ (0xFFFFFFFF<<(offset+width));
    }
    
    outField = (inField & bitMask) >> offset;
    return outField;
}

uint32_t
getBitFieldWidth(uint32_t number)
{
    uint32_t fieldWidth;
    
    number--;
    if (number == 0)
    {
        return 0;
    }
    
    __asm__ volatile ( "bsr %%eax, %%ecx\n\t"
                      : "=c" (fieldWidth)
                      : "a"(number));
    
    return fieldWidth+1;  /* bsr returns the position, we want the width */
}

/*
 * Leaf 2 cache descriptor encodings.
 */
typedef enum {
	_NULL_,		/* NULL (empty) descriptor */
	CACHE,		/* Cache */
	TLB,		/* TLB */
	STLB,		/* Shared second-level unified TLB */
	PREFETCH	/* Prefetch size */
} cpuid_leaf2_desc_type_t;

typedef enum {
	NA,		/* Not Applicable */
	FULLY,		/* Fully-associative */
	TRACE,		/* Trace Cache (P4 only) */
	INST,		/* Instruction TLB */
	DATA,		/* Data TLB */
	DATA0,		/* Data TLB, 1st level */
	DATA1,		/* Data TLB, 2nd level */
	L1,		/* L1 (unified) cache */
	L1_INST,	/* L1 Instruction cache */
	L1_DATA,	/* L1 Data cache */
	L2,		/* L2 (unified) cache */
	L3,		/* L3 (unified) cache */
	L2_2LINESECTOR,	/* L2 (unified) cache with 2 lines per sector */
	L3_2LINESECTOR,	/* L3(unified) cache with 2 lines per sector */
	SMALL,		/* Small page TLB */
	LARGE,		/* Large page TLB */
	BOTH		/* Small and Large page TLB */
} cpuid_leaf2_qualifier_t;

typedef struct cpuid_cache_descriptor {
	uint8_t		value;		/* descriptor code */
	uint8_t		type;		/* cpuid_leaf2_desc_type_t */
	uint8_t		level;		/* level of cache/TLB hierachy */
	uint8_t		ways;		/* wayness of cache */
	uint16_t	size;		/* cachesize or TLB pagesize */
	uint16_t	entries;	/* number of TLB entries or linesize */
} cpuid_cache_descriptor_t;

/*
 * These multipliers are used to encode 1*K .. 64*M in a 16 bit size field
 */
#define	K	(1)
#define	M	(1024)

/*
 * Intel cache descriptor table:
 */
static cpuid_cache_descriptor_t intel_cpuid_leaf2_descriptor_table[] = {
//	-------------------------------------------------------
//	value	type	level		ways	size	entries
//	-------------------------------------------------------
	{ 0x00,	_NULL_,	NA,		NA,	NA,	NA  },
	{ 0x01,	TLB,	INST,		4,	SMALL,	32  },
	{ 0x02,	TLB,	INST,		FULLY,	LARGE,	2   },
	{ 0x03,	TLB,	DATA,		4,	SMALL,	64  },
	{ 0x04,	TLB,	DATA,		4,	LARGE,	8   },
	{ 0x05,	TLB,	DATA1,		4,	LARGE,	32  },
	{ 0x06,	CACHE,	L1_INST,	4,	8*K,	32  },
	{ 0x08,	CACHE,	L1_INST,	4,	16*K,	32  },
	{ 0x09,	CACHE,	L1_INST,	4,	32*K,	64  },
	{ 0x0A,	CACHE,	L1_DATA,	2,	8*K,	32  },
	{ 0x0B,	TLB,	INST,		4,	LARGE,	4   },
	{ 0x0C,	CACHE,	L1_DATA,	4,	16*K,	32  },
	{ 0x0D,	CACHE,	L1_DATA,	4,	16*K,	64  },
	{ 0x0E,	CACHE,	L1_DATA,	6,	24*K,	64  },
	{ 0x21,	CACHE,	L2,		8,	256*K,	64  },
	{ 0x22,	CACHE,	L3_2LINESECTOR,	4,	512*K,	64  },
	{ 0x23,	CACHE,	L3_2LINESECTOR, 8,	1*M,	64  },
	{ 0x25,	CACHE,	L3_2LINESECTOR,	8,	2*M,	64  },
	{ 0x29,	CACHE,	L3_2LINESECTOR, 8,	4*M,	64  },
	{ 0x2C,	CACHE,	L1_DATA,	8,	32*K,	64  },
	{ 0x30,	CACHE,	L1_INST,	8,	32*K,	64  },
	{ 0x40,	CACHE,	L2,		NA,	0,	NA  },
	{ 0x41,	CACHE,	L2,		4,	128*K,	32  },
	{ 0x42,	CACHE,	L2,		4,	256*K,	32  },
	{ 0x43,	CACHE,	L2,		4,	512*K,	32  },
	{ 0x44,	CACHE,	L2,		4,	1*M,	32  },
	{ 0x45,	CACHE,	L2,		4,	2*M,	32  },
	{ 0x46,	CACHE,	L3,		4,	4*M,	64  },
	{ 0x47,	CACHE,	L3,		8,	8*M,	64  },
	{ 0x48,	CACHE,	L2,		12, 	3*M,	64  },
	{ 0x49,	CACHE,	L2,		16,	4*M,	64  },
	{ 0x4A,	CACHE,	L3,		12, 	6*M,	64  },
	{ 0x4B,	CACHE,	L3,		16,	8*M,	64  },
	{ 0x4C,	CACHE,	L3,		12, 	12*M,	64  },
	{ 0x4D,	CACHE,	L3,		16,	16*M,	64  },
	{ 0x4E,	CACHE,	L2,		24,	6*M,	64  },
	{ 0x4F,	TLB,	INST,		NA,	SMALL,	32  },
	{ 0x50,	TLB,	INST,		NA,	BOTH,	64  },
	{ 0x51,	TLB,	INST,		NA,	BOTH,	128 },
	{ 0x52,	TLB,	INST,		NA,	BOTH,	256 },
	{ 0x55,	TLB,	INST,		FULLY,	BOTH,	7   },
	{ 0x56,	TLB,	DATA0,		4,	LARGE,	16  },
	{ 0x57,	TLB,	DATA0,		4,	SMALL,	16  },
	{ 0x59,	TLB,	DATA0,		FULLY,	SMALL,	16  },
	{ 0x5A,	TLB,	DATA0,		4,	LARGE,	32  },
	{ 0x5B,	TLB,	DATA,		NA,	BOTH,	64  },
	{ 0x5C,	TLB,	DATA,		NA,	BOTH,	128 },
	{ 0x5D,	TLB,	DATA,		NA,	BOTH,	256 },
	{ 0x60,	CACHE,	L1,		16*K,	8,	64  },
	{ 0x61,	CACHE,	L1,		4,	8*K,	64  },
	{ 0x62,	CACHE,	L1,		4,	16*K,	64  },
	{ 0x63,	CACHE,	L1,		4,	32*K,	64  },
	{ 0x70,	CACHE,	TRACE,		8,	12*K,	NA  },
	{ 0x71,	CACHE,	TRACE,		8,	16*K,	NA  },
	{ 0x72,	CACHE,	TRACE,		8,	32*K,	NA  },
	{ 0x76,	TLB,	INST,		NA,	BOTH,	8   },
	{ 0x78,	CACHE,	L2,		4,	1*M,	64  },
	{ 0x79,	CACHE,	L2_2LINESECTOR,	8,	128*K,	64  },
	{ 0x7A,	CACHE,	L2_2LINESECTOR,	8,	256*K,	64  },
	{ 0x7B,	CACHE,	L2_2LINESECTOR,	8,	512*K,	64  },
	{ 0x7C,	CACHE,	L2_2LINESECTOR,	8,	1*M,	64  },
	{ 0x7D,	CACHE,	L2,		8,	2*M,	64  },
	{ 0x7F,	CACHE,	L2,		2,	512*K,	64  },
	{ 0x80,	CACHE,	L2,		8,	512*K,	64  },
	{ 0x82,	CACHE,	L2,		8,	256*K,	32  },
	{ 0x83,	CACHE,	L2,		8,	512*K,	32  },
	{ 0x84,	CACHE,	L2,		8,	1*M,	32  },
	{ 0x85,	CACHE,	L2,		8,	2*M,	32  },
	{ 0x86,	CACHE,	L2,		4,	512*K,	64  },
	{ 0x87,	CACHE,	L2,		8,	1*M,	64  },
	{ 0xB0,	TLB,	INST,		4,	SMALL,	128 },
	{ 0xB1,	TLB,	INST,		4,	LARGE,	8   },
	{ 0xB2,	TLB,	INST,		4,	SMALL,	64  },
	{ 0xB3,	TLB,	DATA,		4,	SMALL,	128 },
	{ 0xB4,	TLB,	DATA1,		4,	SMALL,	256 },
	{ 0xB5,	TLB,	DATA1,		8,	SMALL,	64  },
	{ 0xB6,	TLB,	DATA1,		8,	SMALL,	128 },
	{ 0xBA,	TLB,	DATA1,		4,	BOTH,	64  },
	{ 0xC1,	STLB,	DATA1,		8,	SMALL,	1024},
	{ 0xCA,	STLB,	DATA1,		4,	SMALL,	512 },
	{ 0xD0,	CACHE,	L3,		4,	512*K,	64  },
	{ 0xD1,	CACHE,	L3,		4,	1*M,	64  },
	{ 0xD2,	CACHE,	L3,		4,	2*M,	64  },
	{ 0xD3,	CACHE,	L3,		4,	4*M,	64  },
	{ 0xD4,	CACHE,	L3,		4,	8*M,	64  },
	{ 0xD6,	CACHE,	L3,		8,	1*M,	64  },
	{ 0xD7,	CACHE,	L3,		8,	2*M,	64  },
	{ 0xD8,	CACHE,	L3,		8,	4*M,	64  },
	{ 0xD9,	CACHE,	L3,		8,	8*M,	64  },
	{ 0xDA,	CACHE,	L3,		8,	12*M,	64  },
	{ 0xDC,	CACHE,	L3,		12, 	1536*K,	64  },
	{ 0xDD,	CACHE,	L3,		12, 	3*M,	64  },
	{ 0xDE,	CACHE,	L3,		12, 	6*M,	64  },
	{ 0xDF,	CACHE,	L3,		12,	12*M,	64  },
	{ 0xE0,	CACHE,	L3,		12,	18*M,	64  },
	{ 0xE2,	CACHE,	L3,		16,	2*M,	64  },
	{ 0xE3,	CACHE,	L3,		16,	4*M,	64  },
	{ 0xE4,	CACHE,	L3,		16,	8*M,	64  },
	{ 0xE5,	CACHE,	L3,		16,	16*M,	64  },
	{ 0xE6,	CACHE,	L3,		16,	24*M,	64  },
	{ 0xF0,	PREFETCH, NA,		NA,	64,	NA  },
	{ 0xF1,	PREFETCH, NA,		NA,	128,	NA  },
	{ 0xFF,	CACHE,  NA,		NA,	0,	NA  }
};
#define	INTEL_LEAF2_DESC_NUM (sizeof(intel_cpuid_leaf2_descriptor_table) / \
				sizeof(cpuid_cache_descriptor_t))

static inline cpuid_cache_descriptor_t *
cpuid_leaf2_find(uint8_t value)
{
	unsigned int	i;

	for (i = 0; i < INTEL_LEAF2_DESC_NUM; i++)
		if (intel_cpuid_leaf2_descriptor_table[i].value == value)
			return &intel_cpuid_leaf2_descriptor_table[i];
	return NULL;
}

/*
 * CPU identification routines.
 */

static i386_cpu_info_t    cpuid_cpu_info;
static i386_cpu_info_t    *cpuid_cpu_infop = NULL;

static void cpuid_fn(uint32_t selector, uint32_t *result)
{
    do_cpuid(selector, result);
}

/* Sinetek: reimplemented, based on AnV, mercurySquad, thanks go to them.
 * Function is AMD-specific.
 */
static void
cpuid_set_AMDcache_info( i386_cpu_info_t * info_p )
{
    uint32_t    reg[4];
    uint32_t    linesizes[LCACHE_MAX];
    cache_type_t    type;
    uint32_t    colors;
    uint64_t cores = 0;
    uint64_t logical = 0;
    
    bzero( linesizes, sizeof(linesizes) );
    
    /* get number of cores in processor */
    /* No HT on AMD so logicals = cores */
    cpuid_fn(0x80000008, reg);
    info_p->cpuid_cores_per_package = bitfield32(reg[ecx], 7, 0) + 1;
    info_p->cpuid_logical_per_package = info_p->cpuid_cores_per_package;
    
    /* L1 Data */
    {
        type = L1D;
        cpuid_fn(0x80000005, reg);
        uint32_t cpuid_c_linesize    = bitfield32(reg[ecx], 7,  0);
        uint32_t cpuid_c_partitions    = bitfield32(reg[ecx], 15, 8);
        uint32_t cpuid_c_associativity    = bitfield32(reg[ecx], 23, 16);
        uint32_t cpuid_c_size        = bitfield32(reg[ecx], 31, 24);
        
        uint32_t cache_associativity    = cpuid_c_associativity;
        
        // size reported in KB.
        info_p->cache_size[type]      = cpuid_c_size * 1024;
        info_p->cache_sharing[type]     = 1;
        info_p->cache_partitions[type]    = cpuid_c_partitions;
        
        linesizes[type] = cpuid_c_linesize;
        uint32_t cache_sets = info_p->cache_size[type] / (cpuid_c_partitions * cpuid_c_linesize * cache_associativity);
        
        colors = ( cpuid_c_linesize * cache_sets ) >> 12;
        if ( colors > vm_cache_geometry_colors )
            vm_cache_geometry_colors = colors;
    }
    /* L1 Instruction */
    {
        type = L1I;
        cpuid_fn(0x80000005, reg);
        uint32_t cpuid_c_linesize    = bitfield32(reg[edx], 7,  0);
        uint32_t cpuid_c_partitions    = bitfield32(reg[edx], 15, 8);
        uint32_t cpuid_c_associativity    = bitfield32(reg[edx], 23, 16);
        uint32_t cpuid_c_size        = bitfield32(reg[edx], 31, 24);
        
        uint32_t cache_associativity    = cpuid_c_associativity;
        
        // size reported in KB.
        info_p->cache_size[type]      = cpuid_c_size * 1024;
        info_p->cache_sharing[type]     = 1;
        info_p->cache_partitions[type]    = cpuid_c_partitions;
        
        linesizes[type] = cpuid_c_linesize;
        uint32_t cache_sets = info_p->cache_size[type] / (cpuid_c_partitions * cpuid_c_linesize * cache_associativity);
        
        colors = ( cpuid_c_linesize * cache_sets ) >> 12;
        if ( colors > vm_cache_geometry_colors )
            vm_cache_geometry_colors = colors;
    }
    /* L2 Unified */
    {
        type = L2U;
        cpuid_fn(0x80000006, reg);
        uint32_t cpuid_c_linesize    = bitfield32(reg[ecx], 7,  0);
        uint32_t cpuid_c_partitions    = bitfield32(reg[ecx], 11, 8);
        uint32_t cpuid_c_associativity    = bitfield32(reg[ecx], 15, 12);
        uint32_t cpuid_c_size        = bitfield32(reg[ecx], 31, 16);
        
        // Special formula for associativity:  2^(assoc / 2)
        uint32_t cache_associativity    = 1ul << (cpuid_c_associativity / 2);
        
        // size reported in KB.
        info_p->cache_size[type]      = cpuid_c_size * 1024;
        info_p->cache_sharing[type]     = 1;
        info_p->cache_partitions[type]    = cpuid_c_partitions;
        
        linesizes[type] = cpuid_c_linesize;
        uint32_t cache_sets = info_p->cache_size[type] / (cpuid_c_partitions * cpuid_c_linesize * cache_associativity);
        
        colors = ( cpuid_c_linesize * cache_sets ) >> 12;
        if ( colors > vm_cache_geometry_colors )
            vm_cache_geometry_colors = colors;
        
        // use for cache size etc.
        info_p->cpuid_cache_L2_associativity = cache_associativity;
        info_p->cpuid_cache_size    = info_p->cache_size[type];
        info_p->cache_linesize        = cpuid_c_linesize;
    }
    /* L3 Unified */
    /* Cache fix from AlGrey */
    {
        type = L3U;
        cpuid_fn(0x80000006, reg);
        uint32_t cpuid_c_linesize    = bitfield32(reg[edx], 7,  0);
        uint32_t cpuid_c_partitions    = bitfield32(reg[edx], 11, 8);
        uint32_t cpuid_c_associativity    = bitfield32(reg[edx], 15, 12);
        uint32_t cpuid_c_size        = bitfield32(reg[edx], 31, 18);
        
        
        DBG(" cpuid_c_linesize            : %d\n", cpuid_c_linesize);
        DBG(" cpuid_c_partitions               : %d\n", cpuid_c_partitions);
        DBG(" cpuid_c_associativity              : %d\n", cpuid_c_associativity);
        DBG(" cpuid_c_size                : %d\n", cpuid_c_size);
        
        // Special formula for associativity:  2^(assoc / 2)
        uint32_t cache_associativity    = 1ul << (cpuid_c_associativity / 2);
        
        if(cpuid_c_size == 0) {
            // no L3
            info_p->cache_size[type]      = 0;
            info_p->cache_sharing[type]     = 0;
            info_p->cache_partitions[type]    = 0;
        } else {
            // size reported in 512 KB packs.
            
            switch (info_p->cpuid_family) {
                case 22:
                    info_p->cache_size[type] = cpuid_c_size * 1024 / (info_p->cpuid_logical_per_package);
                    break;
                case 23:
                    info_p->cache_size[type]      = (512 * 1024 * cpuid_c_size)/(info_p->cpuid_cores_per_package);
                    break;
                default:
                    info_p->cache_size[type]      = cpuid_c_size * 1024;
                    break;
            }
            DBG(" L3             : %d\n", info_p->cache_size[type] );
            
            
            info_p->cache_sharing[type]     = 1;
            info_p->cache_partitions[type]    = cpuid_c_partitions;
            
            linesizes[type] = cpuid_c_linesize;
            uint32_t cache_sets = info_p->cache_size[type] / (cpuid_c_partitions * cpuid_c_linesize * cache_associativity);
            
            colors = ( cpuid_c_linesize * cache_sets ) >> 12;
            if ( colors > vm_cache_geometry_colors )
                vm_cache_geometry_colors = colors;
            }
        }
    }

/* this function is Intel-specific */
static void
cpuid_set_cache_info( i386_cpu_info_t * info_p )
{
    if (IsIntelCPU())
    {
        uint32_t    cpuid_result[4];
        uint32_t    reg[4];
        uint32_t    index;
        uint32_t    linesizes[LCACHE_MAX];
        unsigned int    i;
        unsigned int    j;
        boolean_t    cpuid_deterministic_supported = FALSE;
        
        DBG("cpuid_set_cache_info(%p)\n", info_p);
        
        bzero( linesizes, sizeof(linesizes) );
        
        /* Get processor cache descriptor info using leaf 2.  We don't use
         * this internally, but must publish it for KEXTs.
         */
        cpuid_fn(2, cpuid_result);
        for (j = 0; j < 4; j++) {
            if ((cpuid_result[j] >> 31) == 1)     /* bit31 is validity */
                continue;
            ((uint32_t *) info_p->cache_info)[j] = cpuid_result[j];
        }
        /* first byte gives number of cpuid calls to get all descriptors */
        for (i = 1; i < info_p->cache_info[0]; i++) {
            if (i*16 > sizeof(info_p->cache_info))
                break;
            cpuid_fn(2, cpuid_result);
            for (j = 0; j < 4; j++) {
                if ((cpuid_result[j] >> 31) == 1)
                    continue;
                ((uint32_t *) info_p->cache_info)[4*i+j] =
                cpuid_result[j];
            }
        }
    }
}

static void
cpuid_set_generic_info(i386_cpu_info_t *info_p)
{
    uint32_t    reg[4];
    char            str[128], *p;
    
    DBG("cpuid_set_generic_info(%p)\n", info_p);
    
    /* do cpuid 0 to get vendor */
    cpuid_fn(0, reg);
    info_p->cpuid_max_basic = reg[eax];
    bcopy((char *)&reg[ebx], &info_p->cpuid_vendor[0], 4); /* ug */
    bcopy((char *)&reg[ecx], &info_p->cpuid_vendor[8], 4);
    bcopy((char *)&reg[edx], &info_p->cpuid_vendor[4], 4);
    info_p->cpuid_vendor[12] = 0;
    
    /* get extended cpuid results */
    cpuid_fn(0x80000000, reg);
    info_p->cpuid_max_ext = reg[eax];
    
    /* check to see if we can get brand string */
    if (info_p->cpuid_max_ext >= 0x80000004) {
        /*
         * The brand string 48 bytes (max), guaranteed to
         * be NUL terminated.
         */
        cpuid_fn(0x80000002, reg);
        bcopy((char *)reg, &str[0], 16);
        cpuid_fn(0x80000003, reg);
        bcopy((char *)reg, &str[16], 16);
        cpuid_fn(0x80000004, reg);
        bcopy((char *)reg, &str[32], 16);
        for (p = str; *p != '\0'; p++) {
            if (*p != ' ') break;
        }
        strlcpy(info_p->cpuid_brand_string,
                p, sizeof(info_p->cpuid_brand_string));
        
        if (!strncmp(info_p->cpuid_brand_string, CPUID_STRING_UNKNOWN,
                     min(sizeof(info_p->cpuid_brand_string),
                         strlen(CPUID_STRING_UNKNOWN) + 1))) {
                         /*
                          * This string means we have a firmware-programmable brand string,
                          * and the firmware couldn't figure out what sort of CPU we have.
                          */
                         info_p->cpuid_brand_string[0] = '\0';
                     }
    }
    
    /* Get cache and addressing info. */
    if (info_p->cpuid_max_ext >= 0x80000006) {
        uint32_t assoc;
        cpuid_fn(0x80000006, reg);
        info_p->cpuid_cache_linesize   = bitfield32(reg[ecx], 7, 0);
        assoc = bitfield32(reg[ecx],15,12);
        /*
         * L2 associativity is encoded, though in an insufficiently
         * descriptive fashion, e.g. 24-way is mapped to 16-way.
         * Represent a fully associative cache as 0xFFFF.
         * Overwritten by associativity as determined via CPUID.4
         * if available.
         */
        // addon Bronya rc3 code
        if (assoc == 1)
            assoc = 1;
        else if (assoc == 2)
            assoc = 2;
        else if (assoc == 4)
            assoc = 4;
        else if (assoc == 6)
            assoc = 8;
        else if (assoc == 8)
            assoc = 16;
        else if (assoc == 10)
            assoc = 32;
        else if (assoc == 11)
            assoc = 48;
        else if (assoc == 12)
            assoc = 64;
        else if (assoc == 13)
            assoc = 96;
        else if (assoc == 14)
            assoc = 128;
        else if (assoc == 0xF)
            assoc = 0xFFFF;
        //info_p->cpuid_cache_L2_associativity = assoc;
        info_p->cpuid_cache_L2_associativity = bitfield32(reg[ecx],15,12);
        info_p->cpuid_cache_size       = bitfield32(reg[ecx],31,16);
        /* cpuid_fn(0x80000008, reg);
        info_p->cpuid_address_bits_physical =
        bitfield32(reg[eax], 7, 0);
        info_p->cpuid_address_bits_virtual =
        bitfield32(reg[eax],15, 8); */
    }
    
    
    /*
     * Get processor signature and decode
     * and bracket this with the approved procedure for reading the
     * the microcode version number a.k.a. signature a.k.a. BIOS ID
     */

    if (IsIntelCPU())
    {
        wrmsr64(MSR_IA32_BIOS_SIGN_ID, 0);
        cpuid_fn(1, reg);
        info_p->cpuid_microcode_version =
        (uint32_t) (rdmsr64(MSR_IA32_BIOS_SIGN_ID) >> 32);
    } else {
        cpuid_fn(1, reg);
        info_p->cpuid_microcode_version = 21;
    }
    
    info_p->cpuid_signature = reg[eax];
    info_p->cpuid_stepping  = bitfield32(reg[eax],  3,  0);
    info_p->cpuid_model     = bitfield32(reg[eax],  7,  4);
    info_p->cpuid_family    = bitfield32(reg[eax], 11,  8);
    info_p->cpuid_type      = bitfield32(reg[eax], 13, 12);
    info_p->cpuid_extmodel  = bitfield32(reg[eax], 19, 16);
    info_p->cpuid_extfamily = bitfield32(reg[eax], 27, 20);
    info_p->cpuid_brand     = bitfield32(reg[ebx],  7,  0);
    /** Sinetek: AMD does not like the way the PAT (Page Attribute Table) is set up. **/
    info_p->cpuid_features  = quad(reg[ecx], reg[edx]) & ~CPUID_FEATURE_PAT;
    
    /* Get "processor flag"; necessary for microcode update matching */
    info_p->cpuid_processor_flag = 0;
    
    /* Fold extensions into family/model */
    if (info_p->cpuid_family == 0x0f)
        info_p->cpuid_family += info_p->cpuid_extfamily;
    if (info_p->cpuid_family == 0x0f || info_p->cpuid_family == 0x06)
        info_p->cpuid_model += (info_p->cpuid_extmodel << 4);
    
    if (info_p->cpuid_features & CPUID_FEATURE_HTT & IsIntelCPU())
        info_p->cpuid_logical_per_package =
        bitfield32(reg[ebx], 23, 16);
    else
        info_p->cpuid_logical_per_package = 1;
    
    if (info_p->cpuid_max_ext >= 0x80000001) {
        cpuid_fn(0x80000001, reg);
        if (IsIntelCPU())
        {
            info_p->cpuid_extfeatures =
            quad(reg[ecx], reg[edx]);
        } else {
            /* Sinetek: AMD doesn't like the XD bit. */
            info_p->cpuid_extfeatures =
            quad(reg[ecx], reg[edx]) & ~CPUID_EXTFEATURE_XD;
        }
    }
    
    DBG(" max_basic           : %d\n", info_p->cpuid_max_basic);
    DBG(" max_ext             : 0x%08x\n", info_p->cpuid_max_ext);
    DBG(" vendor              : %s\n", info_p->cpuid_vendor);
    DBG(" brand_string        : %s\n", info_p->cpuid_brand_string);
    DBG(" signature           : 0x%08x\n", info_p->cpuid_signature);
    DBG(" stepping            : %d\n", info_p->cpuid_stepping);
    DBG(" model               : %d\n", info_p->cpuid_model);
    DBG(" family              : %d\n", info_p->cpuid_family);
    DBG(" type                : %d\n", info_p->cpuid_type);
    DBG(" extmodel            : %d\n", info_p->cpuid_extmodel);
    DBG(" extfamily           : %d\n", info_p->cpuid_extfamily);
    DBG(" brand               : %d\n", info_p->cpuid_brand);
    DBG(" features            : 0x%016llx\n", info_p->cpuid_features);
    DBG(" extfeatures         : 0x%016llx\n", info_p->cpuid_extfeatures);
    DBG(" logical_per_package : %d\n", info_p->cpuid_logical_per_package);
    DBG(" microcode_version   : 0x%08x\n", info_p->cpuid_microcode_version);
    
    /* Fold in the Invariant TSC feature bit, if present */
    if (info_p->cpuid_max_ext >= 0x80000007) {
        cpuid_fn(0x80000007, reg);
        info_p->cpuid_extfeatures |=
        reg[edx] & (uint32_t)CPUID_EXTFEATURE_TSCI;
        DBG(" extfeatures         : 0x%016llx\n",
            info_p->cpuid_extfeatures);
    }
    
    if (info_p->cpuid_max_basic >= 0x5) {
        cpuid_mwait_leaf_t    *cmp = &info_p->cpuid_mwait_leaf;
        
        /*
         * Extract the Monitor/Mwait Leaf info:
         */
        cpuid_fn(5, reg);
        cmp->linesize_min = reg[eax];
        cmp->linesize_max = reg[ebx];
        cmp->extensions   = reg[ecx];
        cmp->sub_Cstates  = reg[edx];
        info_p->cpuid_mwait_leafp = cmp;
        
        DBG(" Monitor/Mwait Leaf:\n");
        DBG("  linesize_min : %d\n", cmp->linesize_min);
        DBG("  linesize_max : %d\n", cmp->linesize_max);
        DBG("  extensions   : %d\n", cmp->extensions);
        DBG("  sub_Cstates  : 0x%08x\n", cmp->sub_Cstates);
    }
    
    if (info_p->cpuid_max_basic >= 0x6) {
        cpuid_thermal_leaf_t    *ctp = &info_p->cpuid_thermal_leaf;
        
        /*
         * The thermal and Power Leaf:
         */
        cpuid_fn(6, reg);
        ctp->sensor           = bitfield32(reg[eax], 0, 0);
        ctp->dynamic_acceleration = bitfield32(reg[eax], 1, 1);
        ctp->invariant_APIC_timer = bitfield32(reg[eax], 2, 2);
        ctp->core_power_limits    = bitfield32(reg[eax], 4, 4);
        ctp->fine_grain_clock_mod = bitfield32(reg[eax], 5, 5);
        ctp->package_thermal_intr = bitfield32(reg[eax], 6, 6);
        ctp->thresholds          = bitfield32(reg[ebx], 3, 0);
        ctp->ACNT_MCNT          = bitfield32(reg[ecx], 0, 0);
        ctp->hardware_feedback      = bitfield32(reg[ecx], 1, 1);
        ctp->energy_policy      = bitfield32(reg[ecx], 3, 3);
        info_p->cpuid_thermal_leafp = ctp;
        
        DBG(" Thermal/Power Leaf:\n");
        DBG("  sensor               : %d\n", ctp->sensor);
        DBG("  dynamic_acceleration : %d\n", ctp->dynamic_acceleration);
        DBG("  invariant_APIC_timer : %d\n", ctp->invariant_APIC_timer);
        DBG("  core_power_limits    : %d\n", ctp->core_power_limits);
        DBG("  fine_grain_clock_mod : %d\n", ctp->fine_grain_clock_mod);
        DBG("  package_thermal_intr : %d\n", ctp->package_thermal_intr);
        DBG("  thresholds           : %d\n", ctp->thresholds);
        DBG("  ACNT_MCNT            : %d\n", ctp->ACNT_MCNT);
        DBG("  ACNT2                : %d\n", ctp->hardware_feedback);
        DBG("  energy_policy        : %d\n", ctp->energy_policy);
    }
    
    if (info_p->cpuid_max_basic >= 0xa) {
        cpuid_arch_perf_leaf_t    *capp = &info_p->cpuid_arch_perf_leaf;
        
        /*
         * Architectural Performance Monitoring Leaf:
         */
        cpuid_fn(0xa, reg);
        capp->version        = bitfield32(reg[eax],  7,  0);
        capp->number        = bitfield32(reg[eax], 15,  8);
        capp->width        = bitfield32(reg[eax], 23, 16);
        capp->events_number = bitfield32(reg[eax], 31, 24);
        capp->events        = reg[ebx];
        capp->fixed_number  = bitfield32(reg[edx],  4,  0);
        capp->fixed_width   = bitfield32(reg[edx], 12,  5);
        info_p->cpuid_arch_perf_leafp = capp;
        
        DBG(" Architectural Performance Monitoring Leaf:\n");
        DBG("  version       : %d\n", capp->version);
        DBG("  number        : %d\n", capp->number);
        DBG("  width         : %d\n", capp->width);
        DBG("  events_number : %d\n", capp->events_number);
        DBG("  events        : %d\n", capp->events);
        DBG("  fixed_number  : %d\n", capp->fixed_number);
        DBG("  fixed_width   : %d\n", capp->fixed_width);
    }
    
    if (info_p->cpuid_max_basic >= 0xd) {
        cpuid_xsave_leaf_t    *xsp;
        /*
         * XSAVE Features:
         */
        cpuid_fn(0xd, info_p->cpuid_xsave_leaf.extended_state);
        info_p->cpuid_xsave_leafp = xsp;
        
        DBG(" XSAVE Leaf:\n");
        DBG("  EAX           : 0x%x\n", xsp->extended_state[eax]);
        DBG("  EBX           : 0x%x\n", xsp->extended_state[ebx]);
        DBG("  ECX           : 0x%x\n", xsp->extended_state[ecx]);
        DBG("  EDX           : 0x%x\n", xsp->extended_state[edx]);
    }
    
    if (info_p->cpuid_model >= CPUID_MODEL_IVYBRIDGE) {
        /*
         * Leaf7 Features:
         */
        cpuid_fn(0x7, reg);
        info_p->cpuid_leaf7_features = reg[ebx];
        
        DBG(" Feature Leaf7:\n");
        DBG("  EBX           : 0x%x\n", reg[ebx]);
    }
    
    return;
}

static uint32_t
cpuid_set_cpufamily(i386_cpu_info_t *info_p)
{
    uint32_t cpufamily = CPUFAMILY_UNKNOWN;
    
    switch (info_p->cpuid_family) {
        case 6:
            switch (info_p->cpuid_model) {
                case 15:
                    cpufamily = CPUFAMILY_INTEL_MEROM;
                    break;
                case 23:
                    cpufamily = CPUFAMILY_INTEL_PENRYN;
                    break;
                case CPUID_MODEL_NEHALEM:
                case CPUID_MODEL_FIELDS:
                case CPUID_MODEL_DALES:
                case CPUID_MODEL_NEHALEM_EX:
                    cpufamily = CPUFAMILY_INTEL_NEHALEM;
                    break;
                case CPUID_MODEL_DALES_32NM:
                case CPUID_MODEL_WESTMERE:
                case CPUID_MODEL_WESTMERE_EX:
                    cpufamily = CPUFAMILY_INTEL_WESTMERE;
                    break;
                case CPUID_MODEL_SANDYBRIDGE:
                case CPUID_MODEL_JAKETOWN:
                    cpufamily = CPUFAMILY_INTEL_SANDYBRIDGE;
                    break;
                case CPUID_MODEL_IVYBRIDGE:
                case CPUID_MODEL_IVYBRIDGE_EP:
                    cpufamily = CPUFAMILY_INTEL_IVYBRIDGE;
                    break;
                case CPUID_MODEL_HASWELL:
                case CPUID_MODEL_HASWELL_EP:
                case CPUID_MODEL_HASWELL_ULT:
                case CPUID_MODEL_CRYSTALWELL:
                    cpufamily = CPUFAMILY_INTEL_HASWELL;
                    break;
                case CPUID_MODEL_BROADWELL:
                case CPUID_MODEL_BRYSTALWELL:
                    cpufamily = CPUFAMILY_INTEL_BROADWELL;
                    break;
                case CPUID_MODEL_SKYLAKE:
                case CPUID_MODEL_SKYLAKE_DT:
                    cpufamily = CPUFAMILY_INTEL_SKYLAKE;
                    break;
            }
            break;
    }
    
    info_p->cpuid_cpufamily = cpufamily;
    DBG("cpuid_set_cpufamily(%p) returning 0x%x\n", info_p, cpufamily);
    
    /* AnV - Fix AMD CPU Family to Intel Penryn */
    /** This is needed to boot because the dyld assumes that an UNKNOWN
     ** Platform is HASWELL-capable, dropping an SSE4.2 'pcmpistri' on us during bcopies.
     **/
    if (IsAmdCPU())
    {
        cpufamily = CPUFAMILY_INTEL_PENRYN;
        info_p->cpuid_cpufamily = cpufamily;
    }
    
    return cpufamily;
}

/* AnV: AMD TLB Fix */
void
FixAMDTLB(void)
{
    uint64_t value = 0;
    
    // re-enable TLB caching if BIOS disabled it
    // MSR_K8_HWCR mod
    value = rdmsr64(0xC0010015);
    value &= ~(1UL << 3);
    wrmsr64(0xC0010015, value);
    
    // MSR_C0011023 mod
    value = rdmsr64(0xC0011023);
    value &= ~(1UL << 1);
    wrmsr64(0xC0011023, value);
}

/*
 * Must be invoked either when executing single threaded, or with
 * independent synchronization.
 */
void
cpuid_set_info(void)
{
    i386_cpu_info_t        *info_p = &cpuid_cpu_info;
    
    cpuid_set_generic_info(info_p);
    
    /* verify we are running on a supported CPU */
    /*if ((strncmp(CPUID_VID_INTEL, info_p->cpuid_vendor,
     min(strlen(CPUID_STRING_UNKNOWN) + 1,
     sizeof(info_p->cpuid_vendor)))) ||
     (cpuid_set_cpufamily(info_p) == CPUFAMILY_UNKNOWN))
     panic("Unsupported CPU");*/
    cpuid_set_cpufamily(info_p);
    
    info_p->cpuid_cpu_type = CPU_TYPE_X86;
    info_p->cpuid_cpu_subtype = CPU_SUBTYPE_X86_ARCH1;
    /* Must be invoked after set_generic_info */
    /* check if running on AMD, call right cache info function */
    if(!strncmp(CPUID_VID_AMD, info_p->cpuid_vendor,
                min(strlen(CPUID_STRING_UNKNOWN) + 1,
                    sizeof(info_p->cpuid_vendor)))) {
                    cpuid_set_AMDcache_info(info_p);
                } else cpuid_set_cache_info(info_p);
    
    /*
     * Find the number of enabled cores and threads
     * (which determines whether SMT/Hyperthreading is active).
     */
    switch (info_p->cpuid_cpufamily) {
        case CPUFAMILY_INTEL_WESTMERE: {
            uint64_t msr = rdmsr64(MSR_CORE_THREAD_COUNT);
            info_p->core_count   = bitfield32((uint32_t)msr, 19, 16);
            info_p->thread_count = bitfield32((uint32_t)msr, 15,  0);
            break;
        }
        case CPUFAMILY_INTEL_HASWELL:
        case CPUFAMILY_INTEL_IVYBRIDGE:
        case CPUFAMILY_INTEL_SANDYBRIDGE:
        case CPUFAMILY_INTEL_NEHALEM: {
            uint64_t msr = rdmsr64(MSR_CORE_THREAD_COUNT);
            info_p->core_count   = bitfield32((uint32_t)msr, 31, 16);
            info_p->thread_count = bitfield32((uint32_t)msr, 15,  0);
            break;
        }
    }
    if (info_p->core_count == 0) {
        info_p->core_count   = info_p->cpuid_cores_per_package;
        info_p->thread_count = info_p->cpuid_logical_per_package;
    }
    DBG("cpuid_set_info():\n");
    DBG("  core_count   : %d\n", info_p->core_count);
    DBG("  thread_count : %d\n", info_p->thread_count);
    
    info_p->cpuid_model_string = ""; /* deprecated */
}

static struct table {
    uint64_t    mask;
    const char    *name;
} feature_map[] = {
    {CPUID_FEATURE_FPU,       "FPU"},
    {CPUID_FEATURE_VME,       "VME"},
    {CPUID_FEATURE_DE,        "DE"},
    {CPUID_FEATURE_PSE,       "PSE"},
    {CPUID_FEATURE_TSC,       "TSC"},
    {CPUID_FEATURE_MSR,       "MSR"},
    {CPUID_FEATURE_PAE,       "PAE"},
    {CPUID_FEATURE_MCE,       "MCE"},
    {CPUID_FEATURE_CX8,       "CX8"},
    {CPUID_FEATURE_APIC,      "APIC"},
    {CPUID_FEATURE_SEP,       "SEP"},
    {CPUID_FEATURE_MTRR,      "MTRR"},
    {CPUID_FEATURE_PGE,       "PGE"},
    {CPUID_FEATURE_MCA,       "MCA"},
    {CPUID_FEATURE_CMOV,      "CMOV"},
    {CPUID_FEATURE_PAT,       "PAT"},
    {CPUID_FEATURE_PSE36,     "PSE36"},
    {CPUID_FEATURE_PSN,       "PSN"},
    {CPUID_FEATURE_CLFSH,     "CLFSH"},
    {CPUID_FEATURE_DS,        "DS"},
    {CPUID_FEATURE_ACPI,      "ACPI"},
    {CPUID_FEATURE_MMX,       "MMX"},
    {CPUID_FEATURE_FXSR,      "FXSR"},
    {CPUID_FEATURE_SSE,       "SSE"},
    {CPUID_FEATURE_SSE2,      "SSE2"},
    {CPUID_FEATURE_SS,        "SS"},
    {CPUID_FEATURE_HTT,       "HTT"},
    {CPUID_FEATURE_TM,        "TM"},
    {CPUID_FEATURE_PBE,       "PBE"},
    {CPUID_FEATURE_SSE3,      "SSE3"},
    {CPUID_FEATURE_PCLMULQDQ, "PCLMULQDQ"},
    {CPUID_FEATURE_DTES64,    "DTES64"},
    {CPUID_FEATURE_MONITOR,   "MON"},
    {CPUID_FEATURE_DSCPL,     "DSCPL"},
    {CPUID_FEATURE_VMX,       "VMX"},
    {CPUID_FEATURE_SMX,       "SMX"},
    {CPUID_FEATURE_EST,       "EST"},
    {CPUID_FEATURE_TM2,       "TM2"},
    {CPUID_FEATURE_SSSE3,     "SSSE3"},
    {CPUID_FEATURE_CID,       "CID"},
    {CPUID_FEATURE_FMA,       "FMA"},
    {CPUID_FEATURE_CX16,      "CX16"},
    {CPUID_FEATURE_xTPR,      "TPR"},
    {CPUID_FEATURE_PDCM,      "PDCM"},
    {CPUID_FEATURE_SSE4_1,    "SSE4.1"},
    {CPUID_FEATURE_SSE4_2,    "SSE4.2"},
    {CPUID_FEATURE_x2APIC,    "x2APIC"},
    {CPUID_FEATURE_MOVBE,     "MOVBE"},
    {CPUID_FEATURE_POPCNT,    "POPCNT"},
    {CPUID_FEATURE_AES,       "AES"},
    {CPUID_FEATURE_VMM,       "VMM"},
    {CPUID_FEATURE_PCID,      "PCID"},
    {CPUID_FEATURE_XSAVE,     "XSAVE"},
    {CPUID_FEATURE_OSXSAVE,   "OSXSAVE"},
    {CPUID_FEATURE_SEGLIM64,  "SEGLIM64"},
    {CPUID_FEATURE_TSCTMR,    "TSCTMR"},
    {CPUID_FEATURE_AVX1_0,    "AVX1.0"},
    {CPUID_FEATURE_RDRAND,    "RDRAND"},
    {CPUID_FEATURE_F16C,      "F16C"},
    {0, 0}
},
extfeature_map[] = {
    {CPUID_EXTFEATURE_SYSCALL, "SYSCALL"},
    {CPUID_EXTFEATURE_XD,      "XD"},
    {CPUID_EXTFEATURE_1GBPAGE, "1GBPAGE"},
    {CPUID_EXTFEATURE_EM64T,   "EM64T"},
    {CPUID_EXTFEATURE_LAHF,    "LAHF"},
    {CPUID_EXTFEATURE_LZCNT,   "LZCNT"},
    {CPUID_EXTFEATURE_PREFETCHW, "PREFETCHW"},
    {CPUID_EXTFEATURE_RDTSCP,  "RDTSCP"},
    {CPUID_EXTFEATURE_TSCI,    "TSCI"},
    {0, 0}
    
},
leaf7_feature_map[] = {
    {CPUID_LEAF7_FEATURE_SMEP,     "SMEP"},
    {CPUID_LEAF7_FEATURE_ERMS,     "ERMS"},
    {CPUID_LEAF7_FEATURE_RDWRFSGS, "RDWRFSGS"},
    {CPUID_LEAF7_FEATURE_TSCOFF,   "TSC_THREAD_OFFSET"},
    {CPUID_LEAF7_FEATURE_BMI1,     "BMI1"},
    {CPUID_LEAF7_FEATURE_HLE,      "HLE"},
    {CPUID_LEAF7_FEATURE_AVX2,     "AVX2"},
    {CPUID_LEAF7_FEATURE_BMI2,     "BMI2"},
    {CPUID_LEAF7_FEATURE_INVPCID,  "INVPCID"},
    {CPUID_LEAF7_FEATURE_RTM,      "RTM"},
    {CPUID_LEAF7_FEATURE_SMAP,     "SMAP"},
    {CPUID_LEAF7_FEATURE_RDSEED,   "RDSEED"},
    {CPUID_LEAF7_FEATURE_ADX,      "ADX"},
    {CPUID_LEAF7_FEATURE_IPT,      "IPT"},
    {CPUID_LEAF7_FEATURE_SGX,      "SGX"},
    {CPUID_LEAF7_FEATURE_PQM,      "PQM"},
    {CPUID_LEAF7_FEATURE_FPU_CSDS, "FPU_CSDS"},
    {CPUID_LEAF7_FEATURE_MPX,      "MPX"},
    {CPUID_LEAF7_FEATURE_PQE,      "PQE"},
    {CPUID_LEAF7_FEATURE_CLFSOPT,  "CLFSOPT"},
    {CPUID_LEAF7_FEATURE_SHA,      "SHA"},
    {0, 0}
};

static char *
cpuid_get_names(struct table *map, uint64_t bits, char *buf, unsigned buf_len)
{
    size_t    len = 0;
    char    *p = buf;
    int    i;
    
    for (i = 0; map[i].mask != 0; i++) {
        if ((bits & map[i].mask) == 0)
            continue;
        if (len && ((size_t) (p - buf) < (buf_len - 1)))
            *p++ = ' ';
        len = min(strlen(map[i].name), (size_t)((buf_len-1)-(p-buf)));
        if (len == 0)
            break;
        bcopy(map[i].name, p, len);
        p += len;
    }
    *p = '\0';
    return buf;
}

i386_cpu_info_t    *
cpuid_info(void)
{
    /* Set-up the cpuid_info stucture lazily */
    if (cpuid_cpu_infop == NULL) {
        PE_parse_boot_argn("-cpuid", &cpuid_dbg, sizeof(cpuid_dbg));
        cpuid_set_info();
        cpuid_cpu_infop = &cpuid_cpu_info;
    }
    return cpuid_cpu_infop;
}

char *
cpuid_get_feature_names(uint64_t features, char *buf, unsigned buf_len)
{
    size_t    len = 0;
    char    *p = buf;
    int    i;
    
    for (i = 0; feature_map[i].mask != 0; i++) {
        if ((features & feature_map[i].mask) == 0)
            continue;
        if (len && ((size_t)(p - buf) < (buf_len - 1)))
            *p++ = ' ';
        
        len = min(strlen(feature_map[i].name), (size_t) ((buf_len-1) - (p-buf)));
        if (len == 0)
            break;
        bcopy(feature_map[i].name, p, len);
        p += len;
    }
    *p = '\0';
    return buf;
}

char *
cpuid_get_extfeature_names(uint64_t extfeatures, char *buf, unsigned buf_len)
{
    size_t    len = 0;
    char    *p = buf;
    int    i;
    
    for (i = 0; extfeature_map[i].mask != 0; i++) {
        if ((extfeatures & extfeature_map[i].mask) == 0)
            continue;
        if (len && ((size_t) (p - buf) < (buf_len - 1)))
            *p++ = ' ';
        len = min(strlen(extfeature_map[i].name), (size_t) ((buf_len-1)-(p-buf)));
        if (len == 0)
            break;
        bcopy(extfeature_map[i].name, p, len);
        p += len;
    }
    *p = '\0';
    return buf;
}

char *
cpuid_get_leaf7_feature_names(uint64_t features, char *buf, unsigned buf_len)
{
    return cpuid_get_names(leaf7_feature_map, features, buf, buf_len);
}

void
cpuid_feature_display(
                      const char    *header)
{
    char    buf[320];
    
    kprintf("%s: %s", header,
            cpuid_get_feature_names(cpuid_features(), buf, sizeof(buf)));
    if (cpuid_leaf7_features())
        kprintf(" %s", cpuid_get_leaf7_feature_names(
                                                     cpuid_leaf7_features(), buf, sizeof(buf)));
    kprintf("\n");
    if (cpuid_features() & CPUID_FEATURE_HTT) {
#define s_if_plural(n)    ((n > 1) ? "s" : "")
        kprintf("  HTT: %d core%s per package;"
                " %d logical cpu%s per package\n",
                cpuid_cpu_infop->cpuid_cores_per_package,
                s_if_plural(cpuid_cpu_infop->cpuid_cores_per_package),
                cpuid_cpu_infop->cpuid_logical_per_package,
                s_if_plural(cpuid_cpu_infop->cpuid_logical_per_package));
    }
}

void
cpuid_extfeature_display(
                         const char    *header)
{
    char    buf[256];
    
    kprintf("%s: %s\n", header,
            cpuid_get_extfeature_names(cpuid_extfeatures(),
                                       buf, sizeof(buf)));
}

void
cpuid_cpu_display(
                  const char    *header)
{
    if (cpuid_cpu_infop->cpuid_brand_string[0] != '\0') {
        kprintf("%s: %s\n", header, cpuid_cpu_infop->cpuid_brand_string);
    }
}

unsigned int
cpuid_family(void)
{
    return cpuid_info()->cpuid_family;
}

uint32_t
cpuid_cpufamily(void)
{
    return cpuid_info()->cpuid_cpufamily;
}

cpu_type_t
cpuid_cputype(void)
{
    return cpuid_info()->cpuid_cpu_type;
}

cpu_subtype_t
cpuid_cpusubtype(void)
{
    return cpuid_info()->cpuid_cpu_subtype;
}

uint64_t
cpuid_features(void)
{
    static int checked = 0;
    char    fpu_arg[20] = { 0 };
    
    (void) cpuid_info();
    if (!checked) {
        /* check for boot-time fpu limitations */
        if (PE_parse_boot_argn("_fpu", &fpu_arg[0], sizeof (fpu_arg))) {
            printf("limiting fpu features to: %s\n", fpu_arg);
            if (!strncmp("387", fpu_arg, sizeof("387")) || !strncmp("mmx", fpu_arg, sizeof("mmx"))) {
                printf("no sse or sse2\n");
                cpuid_cpu_infop->cpuid_features &= ~(CPUID_FEATURE_SSE | CPUID_FEATURE_SSE2 | CPUID_FEATURE_FXSR);
            } else if (!strncmp("sse", fpu_arg, sizeof("sse"))) {
                printf("no sse2\n");
                cpuid_cpu_infop->cpuid_features &= ~(CPUID_FEATURE_SSE2);
            }
        }
        checked = 1;
    }
    return cpuid_cpu_infop->cpuid_features;
}

uint64_t
cpuid_extfeatures(void)
{
    return cpuid_info()->cpuid_extfeatures;
}

uint64_t
cpuid_leaf7_features(void)
{
    return cpuid_info()->cpuid_leaf7_features;
}

static i386_vmm_info_t    *_cpuid_vmm_infop = NULL;
static i386_vmm_info_t    _cpuid_vmm_info;

static void
cpuid_init_vmm_info(i386_vmm_info_t *info_p)
{
    uint32_t    reg[4];
    uint32_t    max_vmm_leaf;
    
    /* bzero(info_p, sizeof(*info_p));
     
     if (!cpuid_vmm_present())
     return; */
    
    DBG("cpuid_init_vmm_info(%p)\n", info_p);
    
    /* do cpuid 0x40000000 to get VMM vendor */
    cpuid_fn(0x40000000, reg);
    max_vmm_leaf = reg[eax];
    bcopy((char *)&reg[ebx], &info_p->cpuid_vmm_vendor[0], 4);
    bcopy((char *)&reg[ecx], &info_p->cpuid_vmm_vendor[4], 4);
    bcopy((char *)&reg[edx], &info_p->cpuid_vmm_vendor[8], 4);
    info_p->cpuid_vmm_vendor[12] = '\0';
    /*
     if (0 == strcmp(info_p->cpuid_vmm_vendor, CPUID_VMM_ID_VMWARE)) {
     info_p->cpuid_vmm_family = CPUID_VMM_FAMILY_VMWARE;
     } else if (0 == strcmp(info_p->cpuid_vmm_vendor, CPUID_VMM_ID_PARALLELS)) {
     info_p->cpuid_vmm_family = CPUID_VMM_FAMILY_PARALLELS;
     } else {
     info_p->cpuid_vmm_family = CPUID_VMM_FAMILY_UNKNOWN;
     } */
    info_p->cpuid_vmm_family = CPUID_VMM_FAMILY_VMWARE;
    /* VMM generic leaves: https://lkml.org/lkml/2008/10/1/246 */
    if (max_vmm_leaf >= 0x40000010) {
        cpuid_fn(0x40000010, reg);
        
        info_p->cpuid_vmm_tsc_frequency = reg[eax];
        info_p->cpuid_vmm_bus_frequency = reg[ebx];
    }
    
    DBG(" vmm_vendor          : %s\n", info_p->cpuid_vmm_vendor);
    DBG(" vmm_family          : %u\n", info_p->cpuid_vmm_family);
    DBG(" vmm_bus_frequency   : %u\n", info_p->cpuid_vmm_bus_frequency);
    DBG(" vmm_tsc_frequency   : %u\n", info_p->cpuid_vmm_tsc_frequency);
}

boolean_t
cpuid_vmm_present(void)
{
    return (cpuid_features() & CPUID_FEATURE_VMM) ? TRUE : FALSE;
}

i386_vmm_info_t *
cpuid_vmm_info(void)
{
    if (_cpuid_vmm_infop == NULL) {
        cpuid_init_vmm_info(&_cpuid_vmm_info);
        _cpuid_vmm_infop = &_cpuid_vmm_info;
    }
    return _cpuid_vmm_infop;
}

uint32_t
cpuid_vmm_family(void)
{
    return cpuid_vmm_info()->cpuid_vmm_family;
}

