/* SPDX-License-Identifier: GPL-2.0-or-later */

/************************************************************************************************
 *                                                                                              *
 * @file        yf_patchkernel.c                                                                *
 * @see         https://www.ip-phone-forum.de/threads/fritz-os7-openvpn-auf-7590-kein-tun-      *
 *              modul.300433/page-3#post-2309487                                                *
 * @brief       patch kernel instructions while loading this module                             *
 * @version     0.3                                                                             *
 * @author      PeH                                                                             *
 * @date        17.05.2019                                                                      *
 *                                                                                              *
 ************************************************************************************************
 *                                                                                              *
 * Copyright (C) 2019 Peter Haemmerlein (peterpawn@yourfritz.de)                                *
 *                                                                                              *
 ************************************************************************************************
 *                                                                                              *
 * This project is free software, you can redistribute it and/or modify it under the terms of   *
 * the GNU General Public License (version 2) as published by the Free Software Foundation.     *
 *                                                                                              *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;    *
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.    *
 * See the GNU General Public License under https://www.gnu.org/licenses/gpl-2.0.html for more  *
 * details.                                                                                     *
 *                                                                                              *
 ************************************************************************************************
 *                                                                                              *
 * This loadable kernel module looks for machine instructions at specified locations in the     *
 * running kernel and replaces them (in case of a hit) with another instruction (only in-place  *
 * patches are supported).                                                                      *
 *                                                                                              *
 ************************************************************************************************
*/

#ifndef MODULE
#error yf_patchkernel has to be compiled as a loadable kernel module.
#endif

#if ! ( defined __MIPS__ || defined __mips__ )
#error yf_patchkernel supports only MIPS architecture yet.
#endif

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/kallsyms.h>
#include <linux/avm_kernel_config.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Peter Haemmerlein");
MODULE_DESCRIPTION("Patches some forgotten AVM traps on MIPS kernels.");
MODULE_VERSION("0.3");

#define MIPS_NOP       0x00000000 // it's a shift instruction, which does nothing: sll zero, zero, 0
#define MIPS_ADDIU     0x24000000 // add immediate value to RS and store the result in RT
#define MIPS_LW        0x8C000000 // load word from offset to BASE and store it in RT
#define MIPS_TNE       0x00000036 // trap if RS not equal RT
#define MIPS_BASE_MASK 0x03E00000 // base register bits (bits 21 to 26)
#define MIPS_RS_MASK   0x03E00000 // RS register bits (bits 21 to 26) - same as BASE
#define MIPS_RT_MASK   0x001F0000 // RT register bits (bits 16 to 20)
#define MIPS_OFFS_MASK 0x0000FFFF // offset bits in the used instructions (16 bits value)
#define MIPS_BASE_SHFT 21         // base register bits shifted left
#define MIPS_RS_SHFT   21         // RS register bits shifted left
#define MIPS_RT_SHFT   16         // RT register bits shifted left
#define MIPS_REG_V0    2          // register v0
#define MIPS_REG_V1    3          // register v1
#define MIPS_REG_A0    4          // register a0
#define MIPS_TRAP_CODE 0x00000300 // trap code 12 (encoded in bits 6 to 15)
#define MIPS_AND_MASK  0xFFFFFFFF // all bits set for logical AND mask

#define YF_INFO(args...)	pr_info("[%s] ",__this_module.name);pr_cont(args)
#define YF_ERROR(args...)	pr_err("[%s] ",__this_module.name);pr_cont(args)

#ifdef YF_PATCHKERNEL_PROCFS

#include <linux/proc_fs.h>

#define MODULE_PROC_BASE	"yf"
#define MODULE_PROC_NAME	"patchkernel"
#define MODULE_PROC_STATUS	"status"
#define MODULE_PROC_CONTROL	"control"
#define MODULE_PROC_COUNT	"count"
#define MODULE_PROC_LIST	"list"
#define MODULE_PROC_PATCHES	"patch"

/* procfs directory structure used by this module

/proc/yf
      |___ patchkernel
           |___ status               = global status for all patches, only if this is '1', other files are accessible
           |___ control              = global 'kill switch', same syntax as of a single patch below
           |___ count                = number of patch entries
           |___ list                 = a list as overview of all patches
           |___ patch
                |___ <index>         = index number of patch, range 0 to number of patches - 1
                     |___ status     = 0 - disabled, 1 - enabled
                     |___ function   = name of patched function
                     |___ address    = target address patched, if it's enabled)
                     |___ original   = original value, if it's enabled
                     |___ replaced   = new value, if it's enabled
                     |___ control    = enable (1) or disable (1) a patch, it's the only writable file

*/

static unsigned int		globalEnabled = 1;

static struct proc_dir_entry	yf_proc_root = NULL;
static struct proc_dir_entry	yf_proc_base = NULL;
static struct proc_dir_entry	yf_proc_status = NULL;
static struct proc_dir_entry	yf_proc_count = NULL;
static struct proc_dir_entry	yf_proc_summary = NULL;
static struct proc_dir_entry	yf_proc_control = NULL;
static struct proc_dir_entry	yf_proc_patches = NULL;

/* type and index (if needed) are stored as 16-bit integers in a single 32-bit value for 'user data' */

typedef enum readType
{
	globalStatus,
	globalCount,
	globalSummary,
	patchStatus,
	patchFunction,
	patchAddress,
	patchOriginal,
	patchReplaced
} readType_t;

typedef enum writeType
{
	globalControl,
	patchControl
}

#define MAKE_PROCFS_DATA(type,index)    (void *)( ( type << 16 ) + index )
#define GET_PROCFS_DATA_TYPE(data)	(unsigned int)( data >> 16 )
#define GET_PROCFS_DATA_INDEX(data)     (unsigned int)( data & 0xFFFF )

static struct file_operations	r_fops =
{
	read:	procfs_read
}

static struct file_operations	w_fops =
{
	write:	procfs_write
}

#endif /* ifdef YF_PATCHKERNEL_PROCFS */

typedef struct patchEntry
{
	unsigned char		*fname;         // kernel symbol name, where to start with a search
	unsigned int		*startAddress;  // the result from kallsyms_lookup_name for the above symbol
	unsigned int		startOffset;    // number of instructions (32 bits per instruction) to skip prior to first comparision
	unsigned int		maxOffset;      // maximum number of instructions to process, while searching for this patch
	unsigned int		lookFor;        // the value to look for, the source value will be modified by AND and OR masks first (see below)
	unsigned int		andMask;        // the mask to use for a logical AND operation, may be used to mask out unwanted bits from value
	unsigned int		orMask;         // the mask to use for a logical OR operation, may be used as a mask to set some additional bits or to ensure, they're set already
	unsigned int		verifyOffset;   // the offset of another value to check, if the search from above was successful, if it's 0, no further check is performed
	unsigned int		verifyValue;    // the expected value from verification, after processing AND and OR operations with masks below
	unsigned int		verifyAndMask;  // the AND mask for verification
	unsigned int		verifyOrMask;   // the OR mask for verification
	unsigned int		patchOffset;    // the offset of instruction to patch, relative to the search result (not to verification offset)
	unsigned int		patchValue;     // the new value to store at patched location
	unsigned int		*patchAddress;  // the address, where the change was applied
	unsigned int		originalValue;  // the original value prior to patching
	int			isPatched;      // not zero, if this patch was applied successfully
#ifdef YF_PATCHKERNEL_PROCFS
	struct proc_dir_entry	*procfs_entry;  // address of procfs subdirectory for this patch
#endif /* ifdef YF_PATCHKERNEL_PROCFS */
} patchEntry_t;

// using the first version number, where a patch will be applied no more, offers the possibility to use 
// this list for unknown, but later released error correction versions, too

typedef struct patchList
{
	unsigned int	majorMin;	// minimal major version, where this patch list may be applied
	unsigned int	minorMin;	// minimal minor version, where this patch list may be applied
	unsigned int	revisionMin; 	// minimal revision number, where this patch list may be applied to	
	unsigned int	majorMax;	// first major version, where this patch list will be applied no more
	unsigned int	minorMax;	// first minor version, where this patch list will be applied no more
	unsigned int	revisionMax; 	// first revision number (if known and not equal zero), where this patch list will be applied no more
	const char 	*patchName;	// patch name to be used for messages
	patchEntry_t	*patches;	// list of patchEntry structures for this patch list entry
} patchList_t;

static unsigned int yf_patchkernel_patch(patchList_t *);
static unsigned int yf_patchkernel_run_patch(patchEntry_t *);
static void yf_patchkernel_restore(patchList_t *);

// entries to patch for TUN device on 7490/75x0 devices, starting with FRITZ!OS version 07.00, up to 07.08

static patchEntry_t	patchesForTunDevice_pre0708[] = {
	{
		.fname = "ip_forward",
		.maxOffset = 10,
		.lookFor = MIPS_LW + (MIPS_REG_A0 << MIPS_BASE_SHFT) + offsetof(struct sk_buff, sk),
		.andMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.patchValue = MIPS_ADDIU + (MIPS_REG_V0 << MIPS_RT_SHFT)
	},
	{
		.fname = "netif_receive_skb",
		.maxOffset = 10,
		.lookFor = MIPS_LW + (MIPS_REG_A0 << MIPS_BASE_SHFT) + offsetof(struct sk_buff, sk),
		.andMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.verifyOffset = 1,
		.verifyValue = MIPS_TNE + MIPS_TRAP_CODE,
		.verifyAndMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.patchOffset = 1,
		.patchValue = MIPS_NOP
	},
	{
		.fname = "__netif_receive_skb",
		.maxOffset = 8,
		.lookFor = MIPS_LW + (MIPS_REG_A0 << MIPS_BASE_SHFT) + offsetof(struct sk_buff, sk),
		.andMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.verifyOffset = 1,
		.verifyValue = MIPS_TNE + MIPS_TRAP_CODE,
		.verifyAndMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.patchOffset = 1,
		.patchValue = MIPS_NOP
	},
	{
		.fname = NULL			// last entry needed as 'end of list' marker
	}
};

// entries to patch for TUN device on 7490/75x0 devices, starting with FRITZ!OS version 07.08

static patchEntry_t	patchesForTunDevice_0708[] = {
	{
		.fname = "ip_forward",
		.maxOffset = 12,
		.lookFor = MIPS_LW + (MIPS_REG_A0 << MIPS_BASE_SHFT) + offsetof(struct sk_buff, sk),
		.andMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.patchValue = MIPS_ADDIU + (MIPS_REG_V0 << MIPS_RT_SHFT)
	},
	{
		.fname = "ip6_forward",
		.startOffset = 15,
		.maxOffset = 10,
		.lookFor = MIPS_LW + (MIPS_REG_A0 << MIPS_BASE_SHFT) + offsetof(struct sk_buff, sk),
		.andMask = MIPS_AND_MASK - MIPS_RT_MASK,
		.patchValue = MIPS_ADDIU + (MIPS_REG_V0 << MIPS_RT_SHFT)
	},
	{
		.fname = NULL		// last entry needed as 'end of list' marker
	}
};

static patchList_t	entries[] = {
	{
		.majorMin = 7,
		.minorMin = 8,
		.revisionMin = 0,
		.majorMax = 0,
		.minorMax = 0,
		.revisionMax = 0,
		.patchName = "patches for TUN device since FRITZ!OS 07.08"
		.patches = patchesForTunDevice_0708
	},
	{
		.majorMin = 6,
		.minorMin = 98,
		.revisionMin = 0,
		.majorMax = 7,
		.minorMax = 8,
		.revisionMax = 0,
		.patchName = "patches for TUN device from FRITZ!OS 06.98 to FRITZ!OS 07.08"
		.patches = patchesForTunDevice_pre0708
	},
	{
		.patches = NULL
	}
};

static unsigned int	patches_applied = 0;	// number of patches applied successfully

static unsigned int yf_patchkernel_run_patch(patchEntry_t *patches)
{
	unsigned int	patches_applied = 0;
	patchEntry_t	*patch = patches;

	unsigned int	*ptr;
	unsigned int	offset;
	unsigned int	value;
	unsigned int	orgValue;
	unsigned int	verify;

	while (patch->fname)
	{
		ptr = (unsigned int *)kallsyms_lookup_name(patch->fname);

		if (!ptr)
		{
			YF_INFO("Unable to locate kernel symbol '%s', patch skipped.\n", patch->fname);
		}
		else
		{
			YF_INFO("Patching kernel function '%s' at address %#010x.\n", patch->fname, (unsigned int)ptr);

			for (offset = 0, patch->startAddress = ptr, ptr += patch->startOffset; offset < patch->maxOffset; offset++, ptr++)
			{
				value = (*ptr & patch->andMask) | patch->orMask;
				orgValue = *(ptr + patch->patchOffset);

				if (orgValue == patch->patchValue)
				{
					YF_INFO("Found patched instruction (%#010x) at address %#010x, looks like this patch was applied already or is not necessary.\n", orgValue, (unsigned int)(ptr + patch->patchOffset));
					break;
				}

				if (value == patch->lookFor)
				{
					if (patch->verifyOffset != 0)
					{
						verify = (*(ptr + patch->verifyOffset) & patch->verifyAndMask) | patch->verifyOrMask;
						if (verify != patch->verifyValue) continue;
					}

					patch->patchAddress = ptr + patch->patchOffset;
					patch->originalValue = *(patch->patchAddress);
					*(patch->patchAddress) = patch->patchValue;
					patch->isPatched = 1;
					patches_applied++;

					YF_INFO("Found instruction to patch (%#010x) at address %#010x, replaced it with %#010x.\n", patch->originalValue, (unsigned int)(patch->patchAddress), *(patch->patchAddress));

					break;
				}
			}

			if (!(patch->isPatched))
			{
				YF_INFO("No instruction to patch found in function '%s', patch skipped.\n", patch->fname);
			}
		}
		patch++;
	}

	return patches_applied;
}

static void yf_patchkernel_restore(patchList_t *list)
{
	while (list->patches)
	{
		while (patch->fname)
		{
			if (patch->isPatched)
			{
				*(patch->patchAddress) = patch->originalValue;
				patch->isPatched = 0;

				YF_INFO("Reversed patch in '%s' at address %#010x to original value %#010x.\n", patch->fname, (unsigned int)(patch->patchAddress), patch->originalValue);
			}

			patch++;
		}
		list++;
	}
}

static int yf_patchkernel_parse_firmwarestring(int *major, int *minor, int *revision, int *dirty, const char *version)
{
	unsigned int	value;
	char *		ptr;
	char *		conv;
	char		fwString[sizeof(avm_kernel_version_info->firmwarestring)];
	int		state;		// value to parse: 1 - major, 2 - minor, 3 - revision, 4 - dirty, 5 - outside
	char		delim;

	if (avm_kernel_version_info == NULL)
	{
		version = "(avm_kernel_version_info pointer is NULL)";
		return 0;
	}

	if (strlen(avm_kernel_version_info->firmwarestring) == 0)
	{
		version = "(zero length string at avm_kernel_version_info->firmwarestring)";
		return 0;
	}

	version = avm_kernel_version_info->firmwarestring;

	if (strscpy(fwString, avm_kernel_version_info->firmwarestring, sizeof(fwString)) <= 0)
	{
		return 0;
	}

	ptr = fwString;
	state = 1;

	while (*ptr && state < 5)
	{
		conv = ptr;
	
		delim = (state == 3 ? '-' : (state == 4 ? 'M' : '.'));
		ptr = strchrnul(ptr, delim);
	
		if (*ptr)
		{
			if (state < 4)
			{
				*ptr = 0;
				if (kstrtouint(conv, 10, &value) == 0)
				{
					switch (state)
					{
						case 1:
							*major = value;
							break;
	
						case 2:
							*minor = value;
							break;
	
						case 3:
							*revision = value;
							break;
						
						default:
							return 0;
					}
					state++;
				}
				else
				{
					return 0;	// invalid number
				}
				ptr++;
			}
			else
			{
				*dirty = 1;
				state++;
			}

		}
		else
		{
			*dirty = 0;
			if (state < 3) return 0;	// missing major or minor
			if (state < 4) *revision = 0;
		}
	}
	if (*ptr) return 0;				// unexpected data after revision

	return 1;
}

static unsigned int yf_patchkernel_patch(patchList_t *list)
{
	unsigned int	major;
	unsigned int	minor;
	unsigned int	revision;
	unsigned int	dirty;
	const char *	version_string;
	patchList_t	*next;
	unsigned int	patchCount;

	if (yf_patchkernel_parse_firmwarestring(&major, &minor, &revision, &dirty, &version_string))
	{
		patchCount = 0;
		next = list;

		while (*next->patches)
		{
			next = list + 1;		// early increment to be able to use 'continue' at any place in this loop

			if (list->majorMin || list->minorMin || list->revisionMin)
			{
				if (major < list->majorMin) continue;
				if (major == list->majorMin)
				{
					if (minor < list->minorMin) continue;
					if (minor == list->minorMin && list->revisionMin && revision < list->revisionMin) continue;
				}
			}
			
			// lower limit not specified or current version accepted
			
			if (list->majorMax || list->minorMax || list->revisionMax)
			{
				if (major > list->majorMax) continue;
				if (major == list->majorMax)
				{
					if (minor > list->minorMax) continue;
					if (minor == list->minorMax && list->revisionMax && revision > list->revisionMax) continue;
				}
			}
	
			// upper limit not specified or current version accepted

			YF_INFO("Version check was successful for patch list '%s', it will get applied now.\n", list->patchName);

			patchCount += yf_patchkernel_run_patch(list->patches);
		}
	}
	else
	{
		YF_INFO("Unable to parse firmware version string from vendor: %s\n", version_string); 
	}
}

#ifdef YF_PATCHKERNEL_PROCFS

/* procfs handling */

static int yf_patchkernel_init_procfs_root(void)
{
	if (yf_proc_root == NULL)
	{
		yf_proc_root = proc_mkdir(MODULE_PROC_BASE, NULL);
	}

	return (yf_proc_root == NULL ? 1 : 0);
}

static int yf_patchkernel_init_procfs_dir(void)
{
	if (yf_proc_base == NULL)
	{
		(!yf_patchkernel_init_procfs_root())
		{
			yf_proc_base = proc_mkdir(MODULE_PROC_NAME, &yf_proc_root);
		}
	}

	return (yf_proc_base == NULL ? 1 : 0);
}

static int yf_patchkernel_init_procfs_patches(patchEntry *patch)
{
	yf_proc_patches = proc_mkdir(MODULE_PROC_PATCHES, &yf_proc_dir);

	return (yf_proc_base == NULL ? 1 : 0);
}

static int yf_patchkernel_init_procfs(patchEntry *patch)
{
	if (yf_patchkernel_init_procfs_dir()) return 1;

	yf_proc_status = proc_create_data(MODULE_PROC_STATUS, S_IRUSR + S_IRGRP + S_IROTH, yf_proc_base, &r_fops, MAKE_PROCFS_DATA(globalStatus,0));
	if (!yf_proc_status) return 1;

	yf_proc_control = proc_create_data(MODULE_PROC_CONTROL, S_IWUSR, yf_proc_base, &w_fops, MAKE_PROCFS_DATA(globalControl,0));
	if (!yf_proc_control) return 1;

	if (yf_patchkernel_init_procfs_patches(patch)) return 1;

	yf_proc_count = proc_create_data(MODULE_PROC_COUNT, S_IRUSR + S_IRGRP + S_IROTH, yf_proc_base, &r_fops, MAKE_PROCFS_DATA(globalStatus,0));

}

#endif /* ifdef YF_PATCHKERNEL_PROCFS */

/* entry points */

static int __init yf_patchkernel_init(void)
{
	YF_INFO("Initialization started\n");
	YF_INFO("Any preceding error messages regarding memory allocation are expected and may be ignored.\n");

#ifdef YF_PATCHKERNEL_PROCFS

	if (yf_patchkernel_init_procfs(patchesForTunDevice))
	{
		YF_ERROR("Error initializing procfs entries.\n");
		return 1;
	}

#endif /* ifdef YF_PATCHKERNEL_PROCFS */

	patches_applied = yf_patchkernel_patch(patchesForTunDevice);

	YF_INFO("%u patches applied.\n", patches_applied);

	return 0;
}

static void __exit yf_patchkernel_exit(void)
{
	YF_INFO("Module will be removed now.\n");

	yf_patchkernel_restore(patchesForTunDevice);

	YF_INFO("All applied patches have been reversed.\n");
}

module_init(yf_patchkernel_init);
module_exit(yf_patchkernel_exit);
