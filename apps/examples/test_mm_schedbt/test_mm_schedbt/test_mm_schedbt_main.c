/****************************************************************************
 * examples/test_mm_schedbt/test_mm_schedbt_main.c
 *
 * Demonstrates CONFIG_MM_BACKTRACE leak detection using the PRECISE backtrace
 * engine: sched_backtrace() (CONFIG_SCHED_BACKTRACE), which walks the EHABI
 * unwind tables (.ARM.exidx) via libgcc's _Unwind_Backtrace.
 *
 * Unlike the test_mm_backtrace example (heuristic stack scan), this needs no
 * app-text address window and no BL/BLX guessing — the unwinder follows the
 * compiler-generated frame descriptions exactly.  Build the code with
 * -funwind-tables (turned on by CONFIG_SCHED_BACKTRACE).
 *
 *  [A] heapinfo_parse_heap  - alloc_call_addr (1 level, built-in)
 *  [B] sched_backtrace()    - captured at alloc time (this file)
 *  [C] allocnode->backtrace - stored in the node by mm via sched_backtrace()
 ****************************************************************************/

#include <tinyara/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <tinyara/mm/mm.h>
#include <tinyara/sched.h>

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
 * leak_alloc: malloc + capture backtrace at the point of allocation.
 *
 * sched_backtrace(getpid(), ...) returns the current thread's call stack via
 * the EHABI unwinder.  skip=0 -> nearest return address is leak_alloc's caller
 * (scenario_a/b), giving the full leak path.  noinline keeps each frame real.
 ****************************************************************************/

static void *__attribute__((noinline)) leak_alloc(size_t size)
{
	void *ptr = malloc(size);

	if (ptr && g_leak_cnt < MAX_TRACKED) {
		struct tracked_alloc_s *t = &g_leaks[g_leak_cnt++];
		t->ptr      = ptr;
		t->size     = size;
		t->bt_depth = sched_backtrace(getpid(), t->bt, BT_DEPTH, 0);
	}

	return ptr;  /* intentionally not freed -> leak */
}

/****************************************************************************
 * Call chain: 3 levels deep so backtrace depth matters
 *
 *  test_mm_schedbt_main
 *    +- create_leaks
 *         +- scenario_a  -> leak_alloc(64), leak_alloc(128)
 *         +- scenario_b  -> leak_alloc(256)
 ****************************************************************************/

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

	printf("\n--- [B] sched_backtrace() result (captured at alloc time) ---\n");
	printf("    engine = EHABI unwind (.ARM.exidx via libgcc); depth = %d\n", BT_DEPTH);
	printf("    resolve: arm-none-eabi-addr2line -f -e common_dbg <addr>...\n\n");

	for (i = 0; i < g_leak_cnt; i++) {
		printf("  leak[%d]  ptr=%p  size=%zu  path:", i,
			   g_leaks[i].ptr, g_leaks[i].size);
		if (g_leaks[i].bt_depth <= 0) {
			printf(" (no return addresses found)");
		}
		printf("\n");
		for (j = 0; j < g_leaks[i].bt_depth; j++) {
			printf("    bt[%d] = %p\n", j, g_leaks[i].bt[j]);
		}
	}
}

/****************************************************************************
 * test_mm_schedbt_main
 ****************************************************************************/

#ifdef CONFIG_BUILD_KERNEL
int main(int argc, FAR char *argv[])
#else
int test_mm_schedbt_main(int argc, char *argv[])
#endif
{
	void *clean;

	printf("\n========================================\n");
	printf(" test_mm_schedbt  (PID: %d)\n", getpid());
	printf("========================================\n");
	printf(" CONFIG_MM_BACKTRACE      = %d\n", CONFIG_MM_BACKTRACE);
	printf(" CONFIG_MM_BACKTRACE_SKIP = %d\n", CONFIG_MM_BACKTRACE_SKIP);
	printf(" engine                   = sched_backtrace (EHABI)\n");
	printf("----------------------------------------\n\n");

	/* [1] Create leaks - each call to leak_alloc() captures backtrace */
	printf("[1] Creating leaks (3 call levels deep)...\n");
	create_leaks();

	/* Direct leak from this frame - bt[0] will be in this function */
	leak_alloc(512);

	/* A clean alloc+free - should NOT appear in heapinfo for this PID */
	clean = malloc(32);
	free(clean);

	/* [A] heapinfo: shows alloc_call_addr (1 level, built-in) */
	printf("\n--- [A] heapinfo alloc_call_addr (1-level, built-in) ---\n");
	printf("    Columns: node | size | A/F | alloc_call_addr | pid\n\n");
	heapinfo_parse_heap(BASE_HEAP, HEAPINFO_DETAIL_PID, getpid());

	/* [B] sched_backtrace captured at alloc time */
	print_backtrace_list();

	/* [C] Heap scan: read backtrace[] stored in each allocnode by mm */
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
	printf(" test_mm_schedbt done\n");
	printf("========================================\n\n");

	return 0;
}
