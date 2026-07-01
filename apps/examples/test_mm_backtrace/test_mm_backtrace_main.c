/****************************************************************************
 * examples/test_mm_backtrace/test_mm_backtrace_main.c
 *
 * Demonstrates CONFIG_MM_BACKTRACE by intentionally leaking memory and
 * showing the call-site backtrace in three ways:
 *
 *  [A] heapinfo_parse_heap  - shows alloc_call_addr (1 level, already in TizenRT)
 *  [B] up_backtrace()       - captures N-level trace at alloc time (this file)
 *  [C] allocnode->backtrace - stored in the node itself (requires MM_BACKTRACE port)
 ****************************************************************************/

#include <tinyara/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <tinyara/mm/mm.h>
#include <tinyara/sched.h>

/****************************************************************************
 * User-space backtrace (stack scan)
 *
 * The kernel up_backtrace() (arch/arm/src/armv7-a/arm_backtrace_thumb.c)
 * needs this_task()/CURRENT_REGS/kernel text bounds — none available to a
 * user app in a PROTECTED / APP_BINARY_SEPARATION build.  So instead of the
 * frame-pointer walk, we port the kernel's *branch-scan* heuristic
 * (backtrace_branch): scan the current stack upward, and for every word that
 * points just past a Thumb BL/BLX instruction inside the app text, treat it
 * as a return address.  No frame pointer, no TCB required.
 *
 * This produces the leak's call path; resolve the addresses with:
 *   arm-none-eabi-addr2line -f -e build/output/bin/app1_dbg <addr> ...
 ****************************************************************************/

/* User text (XIP flash) address window used to validate return addresses.
 *
 * IMPORTANT: this range MUST exclude kernel text.  In a PROTECTED build the
 * kernel lives below the user text (~0x0e000000); if we let a kernel address
 * pass the check we then dereference it to inspect the instruction, and a
 * user-mode read of kernel memory raises a Data Abort.
 *
 * The bounds below come from the runtime log line
 *   elf_show_all_bin_section_addr: [common] Text Addr : 0xe260010 ...
 *   elf_show_all_bin_section_addr: [app1]   Text Addr : 0xeaa7030 ...
 * i.e. common text starts at 0x0e260010 and app1 text ends ~0x0ef52630.
 * These are fixed XIP flash addresses for this board/partition layout. */
#define APP_TEXT_START   0x0e260000u	/* start of user (common) text */
#define APP_TEXT_END     0x0ef53000u	/* end of app1 text            */

/* How far up the stack to scan for return addresses (bytes). */
#define BT_STACK_WINDOW  4096

/* Thumb instruction decode masks (same as arm_backtrace_thumb.c) */
#define BT_IMASK_BLX     0xff80u	/* blx  reg          */
#define BT_IOP_BLX       0x4780u
#define BT_IMASK_BL      0xf800u	/* bl   (high half)  */
#define BT_IOP_BL        0xf000u

static inline int bt_in_text(uint32_t addr)
{
	return addr >= APP_TEXT_START && addr < APP_TEXT_END;
}

/* Signature matches the kernel up_backtrace() so callers are unchanged.
 * tcb is ignored (we always trace the current stack). */
int up_backtrace(struct tcb_s *tcb, void **buffer, int size, int skip)
{
	uintptr_t sp;
	uintptr_t limit;
	int i = 0;

	(void)tcb;

	if (!buffer || size <= 0) {
		return 0;
	}

	/* Current stack pointer of the caller's frame */
	__asm__ volatile("mov %0, sp" : "=r"(sp));

	limit = sp + BT_STACK_WINDOW;

	for (; sp < limit && i < size; sp += sizeof(uint32_t)) {
		uint32_t val = *(volatile uint32_t *)sp;
		uintptr_t insn_addr;
		uint16_t ins16;

		if (!bt_in_text(val)) {
			continue;
		}

		/* A return address points to the instruction *after* the call.
		 * Back up to the call instruction (clear Thumb bit, -2). Ensure the
		 * instruction (and the BL high half at -4) stay inside user text so
		 * we never dereference an unmapped/kernel address. */
		insn_addr = (val & ~1u) - 2;
		if (insn_addr < APP_TEXT_START + 2 || insn_addr >= APP_TEXT_END) {
			continue;
		}
		ins16 = *(volatile uint16_t *)insn_addr;

		if ((ins16 & BT_IMASK_BLX) == BT_IOP_BLX) {
			/* blx <reg> : single 16-bit instruction */
			if (skip-- <= 0) {
				buffer[i++] = (void *)val;
			}
		} else if ((ins16 & 0xd000u) == 0xd000u) {
			/* Possible low half of a 32-bit BL; check the high half */
			uint16_t hi = *(volatile uint16_t *)(insn_addr - 2);
			if ((hi & BT_IMASK_BL) == BT_IOP_BL) {
				if (skip-- <= 0) {
					buffer[i++] = (void *)val;
				}
			}
		}
	}

	return i;
}

/****************************************************************************
 * Tracked allocation: stores pointer + backtrace captured at alloc time
 ****************************************************************************/

#define MAX_TRACKED  8
#define BT_DEPTH     CONFIG_MM_BACKTRACE   /* 4 */

struct tracked_alloc_s {
	void   *ptr;
	size_t  size;
	void   *bt[BT_DEPTH];
	int     bt_depth;
};

static struct tracked_alloc_s g_leaks[MAX_TRACKED];
static int g_leak_cnt = 0;

/****************************************************************************
 * leak_alloc: malloc + capture backtrace at the point of allocation
 * skip=1 → skip up_backtrace itself, start from this function's caller
 ****************************************************************************/

static void *__attribute__((noinline)) leak_alloc(size_t size)
{
	void *ptr = malloc(size);

	if (ptr && g_leak_cnt < MAX_TRACKED) {
		struct tracked_alloc_s *t = &g_leaks[g_leak_cnt++];
		t->ptr      = ptr;
		t->size     = size;
		/* skip=0: branch-scan starts at the nearest return address, i.e.
		 * leak_alloc's caller (scenario_a/b), giving the full leak path. */
		t->bt_depth = up_backtrace(NULL, t->bt, BT_DEPTH, 0);
	}

	return ptr;  /* intentionally not freed → leak */
}

/****************************************************************************
 * Call chain: 3 levels deep so backtrace depth matters
 *
 *  test_mm_backtrace_main
 *    └─ create_leaks
 *         ├─ scenario_a  → leak_alloc(64), leak_alloc(128)
 *         └─ scenario_b  → leak_alloc(256)
 ****************************************************************************/

/* noinline: keep each frame real so the branch-scan sees a distinct
 * return address per level (otherwise the compiler collapses the chain). */
static void __attribute__((noinline)) scenario_a(void)
{
	leak_alloc(64);
	leak_alloc(128);
}

static void __attribute__((noinline)) scenario_b(void)
{
	leak_alloc(256);
}

static void __attribute__((noinline)) create_leaks(void)
{
	scenario_a();
	scenario_b();
}

/****************************************************************************
 * print_backtrace_list: print all tracked leaks with their backtraces
 ****************************************************************************/

static void print_backtrace_list(void)
{
	int i;
	int j;

	printf("\n--- [B] up_backtrace() result (captured at alloc time) ---\n");
	printf("    depth = %d  (user-space stack-scan; addresses are return PCs)\n", BT_DEPTH);
	printf("    resolve: arm-none-eabi-addr2line -f -e common_dbg <addr>...\n\n");

	for (i = 0; i < g_leak_cnt; i++) {
		printf("  leak[%d]  ptr=%p  size=%zu  path:", i,
			   g_leaks[i].ptr, g_leaks[i].size);
		if (g_leaks[i].bt_depth == 0) {
			printf(" (no return addresses found)");
		}
		printf("\n");
		for (j = 0; j < g_leaks[i].bt_depth; j++) {
			printf("    bt[%d] = %p\n", j, g_leaks[i].bt[j]);
		}
	}
}

/****************************************************************************
 * test_mm_backtrace_main
 ****************************************************************************/

#ifdef CONFIG_BUILD_KERNEL
int main(int argc, FAR char *argv[])
#else
int test_mm_backtrace_main(int argc, char *argv[])
#endif
{
	void *clean;

	printf("\n========================================\n");
	printf(" test_mm_backtrace  (PID: %d)\n", getpid());
	printf("========================================\n");
	printf(" CONFIG_MM_BACKTRACE      = %d\n", CONFIG_MM_BACKTRACE);
	printf(" CONFIG_MM_BACKTRACE_SKIP = %d\n", CONFIG_MM_BACKTRACE_SKIP);
	printf("----------------------------------------\n\n");

	/* [1] Create leaks — each call to leak_alloc() captures backtrace */
	printf("[1] Creating leaks (3 call levels deep)...\n");
	create_leaks();

	/* Direct leak from this frame — bt[0] will be in this function */
	leak_alloc(512);

	/* A clean alloc+free — should NOT appear in heapinfo for this PID */
	clean = malloc(32);
	free(clean);

	/* [A] heapinfo: shows alloc_call_addr (1 level, already in TizenRT) */
	printf("\n--- [A] heapinfo alloc_call_addr (1-level, built-in) ---\n");
	printf("    Columns: node | size | A/F | alloc_call_addr | pid\n\n");
	heapinfo_parse_heap(BASE_HEAP, HEAPINFO_DETAIL_PID, getpid());

	/* [B] up_backtrace captured at alloc time */
	print_backtrace_list();

	/* [C] Heap scan: read backtrace[] stored in each allocnode */
	printf("\n--- [C] allocnode->backtrace[] (heap scan) ---\n");
#if CONFIG_MM_BACKTRACE > 0
	{
		struct mm_heap_s *heap = BASE_HEAP;
		struct mm_allocnode_s *node;
		int region;
		int found = 0;
		pid_t mypid = getpid();

		mm_takesemaphore(heap);

		for (region = 0; region < CONFIG_KMM_REGIONS; region++) {
			for (node = heap->mm_heapstart[region];
			     node < heap->mm_heapend[region];
			     node = (struct mm_allocnode_s *)((char *)node + node->size)) {

				/* Allocated node belonging to our PID */
				if ((node->preceding & MM_ALLOC_BIT) &&
#ifdef CONFIG_DEBUG_MM_HEAPINFO
				    node->pid == mypid
#else
				    1
#endif
				) {
					int j;
					found++;
					printf("  node=%p size=%-6u seqno=%-4lu\n",
					       node, (unsigned)node->size,
					       (unsigned long)node->seqno);
					for (j = 0; j < CONFIG_MM_BACKTRACE; j++) {
						if (node->backtrace[j] == NULL) {
							break;
						}
						printf("    bt[%d] = %p\n", j, node->backtrace[j]);
					}
				}
			}
		}

		mm_givesemaphore(heap);

		if (!found) {
			printf("  (no allocated nodes found for PID %d)\n", mypid);
		}
	}
#else
	printf("    CONFIG_MM_BACKTRACE <= 0, disabled\n");
#endif

	printf("\n[Expected leaks] 64+128+256+512 = %d bytes\n",
		   64 + 128 + 256 + 512);
	printf("[clean alloc %d bytes should NOT appear above]\n\n", 32);

	printf("========================================\n");
	printf(" test_mm_backtrace done\n");
	printf("========================================\n\n");

	return 0;
}
