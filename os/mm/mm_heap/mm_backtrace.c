/****************************************************************************
 * mm/mm_heap/mm_backtrace.c
 *
 * Weak stub for up_backtrace() used when CONFIG_MM_BACKTRACE > 0.
 *
 * In kernel builds, the arch implementation (arm_backtrace_thumb.c or
 * arm_backtrace_fp.c) provides a strong symbol that overrides this stub.
 * In user-space (libumm) builds, this stub is used and returns 0
 * (no backtrace available from user space).
 ****************************************************************************/

#include <tinyara/config.h>
#include <tinyara/mm/mm.h>

#if CONFIG_MM_BACKTRACE > 0

struct tcb_s;

int __attribute__((weak)) up_backtrace(struct tcb_s *tcb,
					void **buffer, int size, int skip)
{
	(void)tcb;
	(void)skip;

	if (buffer && size > 0) {
		buffer[0] = NULL;
	}

	return 0;
}

#endif /* CONFIG_MM_BACKTRACE > 0 */
