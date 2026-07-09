/****************************************************************************
 *
 * Copyright 2023 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * apps/system/test_memory_info/test_memory_info_main.c
 *
 * A heapinfo-like memory inspection tool.  In addition to the standard
 * heapinfo summary / detail views it adds a "-t" option that dumps, for each
 * task, the list of its live allocations with the full allocation call-stack
 * (captured at malloc time by sched_backtrace()) printed on a single line:
 *
 *   PID=<pid>(<name>) ptr=<addr> size=<n> seq=<s> bt: a <- b <- c
 *
 * Resolve the backtrace addresses with:
 *   arm-none-eabi-addr2line -f -e <elf> <addr> ...
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <tinyara/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <tinyara/mm/mm.h>
#include <tinyara/mminfo.h>
#include <tinyara/fs/ioctl.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int test_memory_info_parse(heapinfo_option_t *options)
{
	int ret;
	int fd;

	fd = open(MMINFO_DRVPATH, O_RDWR);
	if (fd < 0) {
		printf("test_memory_info: cannot open %s (errno %d)\n", MMINFO_DRVPATH, get_errno());
		return ERROR;
	}

	ret = ioctl(fd, MMINFOIOC_PARSE, (unsigned long)options);
	if (ret == ERROR) {
		printf("test_memory_info: ioctl failed (errno %d)\n", get_errno());
	}

	close(fd);
	return ret;
}

static void test_memory_info_usage(void)
{
	printf("\nUsage: test_memory_info [OPTION]\n");
	printf("Display heap allocation information.\n\n");
	printf("Options:\n");
	printf("  -t             Dump per-task allocation list with backtrace (one line each)\n");
	printf("  -a             Show all allocation details (same as heapinfo -a)\n");
	printf("  -f             Show the free list\n");
	printf("  -p PID         Restrict output to the given PID (use with -t or -a)\n");
#ifdef CONFIG_APP_BINARY_SEPARATION
	printf("  -b BIN_NAME    Inspect the heap of the given binary\n");
#endif
	printf("  -k             Inspect the kernel heap (default)\n");
	printf("  -i             Initialize the peak allocated size\n");
	printf("  -h             Show this help\n");
#if !(CONFIG_MM_BACKTRACE > 0)
	printf("\nNOTE: -t requires CONFIG_MM_BACKTRACE > 0 (currently disabled).\n");
#endif
	printf("\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#if CONFIG_MM_BACKTRACE > 0
/****************************************************************************
 * Name: test_memory_info_backtrace
 *
 * Description:
 *   Dump allocation backtraces for the selected heap.  Allocation backtraces
 *   are only recorded for USER (app) heaps: user-space malloc captures the
 *   call-stack via sched_backtrace().  The KERNEL heap in a protected build
 *   deliberately records only a sequence number (EHABI unwind is unsafe in
 *   early-boot / IRQ / IDLE kernel contexts), so it will show "bt: (none)".
 *
 *   When the user does not explicitly pick a heap (-k / -b), inspect the
 *   user app heap(s) where the backtraces actually live.
 ****************************************************************************/
static int test_memory_info_backtrace(heapinfo_option_t *options, bool heap_selected)
{
	options->mode = HEAPINFO_DETAIL_BACKTRACE;

	printf("\n=== Per-Task Allocation Backtrace ===\n");
	if (options->pid == HEAPINFO_PID_ALL) {
		printf("(each line is labelled with its owner task)\n");
	} else {
		printf("(PID %d only)\n", options->pid);
	}

	if (heap_selected) {
		/* Honour the user's explicit -k / -b selection. */
		if (options->heap_type == HEAPINFO_HEAP_TYPE_KERNEL) {
			printf("NOTE: kernel heap does not record backtraces (seqno only).\n");
		}
		printf("\n");
		return test_memory_info_parse(options);
	}

#ifdef CONFIG_APP_BINARY_SEPARATION
	/* Default: walk the user app heap(s) - that is where backtraces exist. */
	{
		int ret = OK;
#ifdef CONFIG_APP1_INFO
		printf("\n[app heap: %s]\n", CONFIG_APP1_BIN_NAME);
		options->heap_type = HEAPINFO_HEAP_TYPE_BINARY;
		strncpy(options->app_name, CONFIG_APP1_BIN_NAME, BIN_NAME_MAX - 1);
		options->app_name[BIN_NAME_MAX - 1] = '\0';
		if (test_memory_info_parse(options) == ERROR) {
			ret = ERROR;
		}
#endif
#ifdef CONFIG_APP2_INFO
		printf("\n[app heap: %s]\n", CONFIG_APP2_BIN_NAME);
		options->heap_type = HEAPINFO_HEAP_TYPE_BINARY;
		strncpy(options->app_name, CONFIG_APP2_BIN_NAME, BIN_NAME_MAX - 1);
		options->app_name[BIN_NAME_MAX - 1] = '\0';
		if (test_memory_info_parse(options) == ERROR) {
			ret = ERROR;
		}
#endif
#if !defined(CONFIG_APP1_INFO) && !defined(CONFIG_APP2_INFO)
		printf("\nNo app heap configured (CONFIG_APPn_INFO). Use -b <name>.\n");
		printf("Falling back to kernel heap (backtraces not recorded there).\n\n");
		ret = test_memory_info_parse(options);
#endif
		return ret;
	}
#else
	/* Flat build: single heap holds user allocations with backtraces. */
	printf("\n");
	return test_memory_info_parse(options);
#endif
}
#endif /* CONFIG_MM_BACKTRACE > 0 */

int test_memory_info_main(int argc, char **argv)
{
	int opt;
	bool backtrace_mode = false;
	bool heap_selected = false;
	heapinfo_option_t options;

	options.heap_type = HEAPINFO_HEAP_TYPE_KERNEL;
	options.mode = HEAPINFO_SIMPLE;
	options.pid = HEAPINFO_PID_ALL;

	optind = -1;
	while ((opt = getopt(argc, argv, "tafp:b:kih")) != ERROR) {
		switch (opt) {
		case 't':
			backtrace_mode = true;
			break;
		case 'a':
			options.mode = HEAPINFO_DETAIL_ALL;
			break;
		case 'f':
			options.mode = HEAPINFO_DETAIL_FREE;
			break;
		case 'p':
			if (strcmp(optarg, "0") == 0 || atoi(optarg) > 0) {
				options.pid = atoi(optarg);
			} else {
				printf("test_memory_info: invalid PID '%s'\n", optarg);
				test_memory_info_usage();
				return ERROR;
			}
			break;
#ifdef CONFIG_APP_BINARY_SEPARATION
		case 'b':
			options.heap_type = HEAPINFO_HEAP_TYPE_BINARY;
			strncpy(options.app_name, optarg, BIN_NAME_MAX - 1);
			options.app_name[BIN_NAME_MAX - 1] = '\0';
			heap_selected = true;
			break;
#endif
		case 'k':
			options.heap_type = HEAPINFO_HEAP_TYPE_KERNEL;
			heap_selected = true;
			break;
		case 'i':
			options.mode = HEAPINFO_INIT_PEAK;
			break;
		case 'h':
		case '?':
		default:
			test_memory_info_usage();
			return (opt == 'h') ? OK : ERROR;
		}
	}

	if (backtrace_mode) {
#if CONFIG_MM_BACKTRACE > 0
		return test_memory_info_backtrace(&options, heap_selected);
#else
		printf("test_memory_info: -t requires CONFIG_MM_BACKTRACE > 0\n");
		return ERROR;
#endif
	}

	if (test_memory_info_parse(&options) == ERROR) {
		return ERROR;
	}

	if (options.mode == HEAPINFO_INIT_PEAK) {
		printf("Peak allocated memory size is cleared\n");
	}

	return OK;
}
