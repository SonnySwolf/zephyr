/** @file
 * @brief Network core definitions
 *
 * Definitions for networking support.
 */

/*
 * Copyright (c) 2015 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __NET_CORE_H
#define __NET_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Network subsystem logging helpers */

#ifdef CONFIG_NETWORKING_WITH_LOGGING
#define NET_DBG(fmt, ...) printk("net: %s (%p): " fmt, __func__, \
				 sys_thread_self_get(), ##__VA_ARGS__)
#define NET_ERR(fmt, ...) printk("net: %s: " fmt, __func__, ##__VA_ARGS__)
#define NET_INFO(fmt, ...) printk("net: " fmt,  ##__VA_ARGS__)
#define NET_PRINT(fmt, ...) printk(fmt, ##__VA_ARGS__)
#else
#define NET_DBG(...)
#define NET_ERR(...)
#define NET_INFO(...)
#define NET_PRINT(...)
#endif /* CONFIG_NETWORKING_WITH_LOGGING */

struct net_buf;
struct net_context;

#include <misc/printk.h>
#include <string.h>

#include <net/net_if.h>

/* Called by lower network stack when a network packet has been received */
int net_recv(struct net_if *iface, struct net_buf *buf);

/** @cond ignore */
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_PRINTK)
#include <offsets.h>
#include <misc/printk.h>

static inline void net_analyze_stack(const char *name,
				     unsigned char *stack,
				     size_t size)
{
	unsigned i, stack_offset, pcnt, unused = 0;

	/* The TCS is always placed on a 4-byte aligned boundary - if
	 * the stack beginning doesn't match that there will be some
	 * unused bytes in the beginning.
	 */
	stack_offset = __tTCS_SIZEOF + ((4 - ((unsigned)stack % 4)) % 4);

/* TODO
 * Currently all supported platforms have stack growth down and there is no
 * Kconfig option to configure it so this always build "else" branch.
 * When support for platform with stack direction up (or configurable direction)
 * is added this check should be confirmed that correct Kconfig option is used.
 */
#if defined(CONFIG_STACK_GROWS_UP)
	for (i = size - 1; i >= stack_offset; i--) {
		if ((unsigned char)stack[i] == 0xaa) {
			unused++;
		} else {
			break;
		}
	}
#else
	for (i = stack_offset; i < size; i++) {
		if ((unsigned char)stack[i] == 0xaa) {
			unused++;
		} else {
			break;
		}
	}
#endif

	/* Calculate the real size reserved for the stack */
	size -= stack_offset;
	pcnt = ((size - unused) * 100) / size;

	printk("net (%p): %s stack real size %u "
	       "unused %u usage %u/%u (%u %%)\n",
	       sys_thread_self_get(), name,
	       size + stack_offset, unused, size - unused, size, pcnt);
}
#else
#define net_analyze_stack(...)
#endif
/* @endcond */

#ifdef __cplusplus
}
#endif

#endif /* __NET_CORE_H */