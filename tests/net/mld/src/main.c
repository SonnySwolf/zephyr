/* main.c - Application main entry point */

/*
 * Copyright (c) 2015 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sections.h>

#include <ztest.h>

#include <net/net_if.h>
#include <net/nbuf.h>
#include <net/net_ip.h>
#include <net/net_core.h>
#include <net/ethernet.h>
#include <net/net_mgmt.h>
#include <net/net_event.h>

#include "icmpv6.h"
#include "ipv6.h"

#define NET_LOG_ENABLED 1
#include "net_private.h"

#if defined(CONFIG_NET_DEBUG_MLD)
#define DBG(fmt, ...) printk(fmt, ##__VA_ARGS__)
#else
#define DBG(fmt, ...)
#endif

static struct in6_addr my_addr = { { { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
				       0, 0, 0, 0, 0, 0, 0, 0x1 } } };
static struct in6_addr mcast_addr = { { { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
					  0, 0, 0, 0, 0, 0, 0, 0x1 } } };

static struct net_if *iface;
static bool is_group_joined;
static bool is_group_left;
static bool is_join_msg_ok;
static bool is_leave_msg_ok;
static struct k_sem wait_data;

#define WAIT_TIME 500
#define WAIT_TIME_LONG MSEC_PER_SEC
#define MY_PORT 1969
#define PEER_PORT 13856

struct net_test_mld {
	uint8_t mac_addr[sizeof(struct net_eth_addr)];
	struct net_linkaddr ll_addr;
};

int net_test_dev_init(struct device *dev)
{
	return 0;
}

static uint8_t *net_test_get_mac(struct device *dev)
{
	struct net_test_mld *context = dev->driver_data;

	if (context->mac_addr[0] == 0x00) {
		/* 10-00-00-00-00 to 10-00-00-00-FF Documentation RFC7042 */
		context->mac_addr[0] = 0x10;
		context->mac_addr[1] = 0x00;
		context->mac_addr[2] = 0x00;
		context->mac_addr[3] = 0x00;
		context->mac_addr[4] = 0x00;
		context->mac_addr[5] = sys_rand32_get();
	}

	return context->mac_addr;
}

static void net_test_iface_init(struct net_if *iface)
{
	uint8_t *mac = net_test_get_mac(net_if_get_device(iface));

	net_if_set_link_addr(iface, mac, sizeof(struct net_eth_addr),
			     NET_LINK_ETHERNET);
}

static int tester_send(struct net_if *iface, struct net_buf *buf)
{
	struct net_icmp_hdr *icmp = NET_ICMP_BUF(buf);

	if (!buf->frags) {
		TC_ERROR("No data to send!\n");
		return -ENODATA;
	}

	if (icmp->type == NET_ICMPV6_MLDv2) {
		/* FIXME, add more checks here */

		is_join_msg_ok = true;
		is_leave_msg_ok = true;

		k_sem_give(&wait_data);
	}

	net_nbuf_unref(buf);

	return 0;
}

struct net_test_mld net_test_data;

static struct net_if_api net_test_if_api = {
	.init = net_test_iface_init,
	.send = tester_send,
};

#define _ETH_L2_LAYER DUMMY_L2
#define _ETH_L2_CTX_TYPE NET_L2_GET_CTX_TYPE(DUMMY_L2)

NET_DEVICE_INIT(net_test_mld, "net_test_mld",
		net_test_dev_init, &net_test_data, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
		&net_test_if_api, _ETH_L2_LAYER, _ETH_L2_CTX_TYPE,
		127);

static void group_joined(struct net_mgmt_event_callback *cb,
			 uint32_t nm_event, struct net_if *iface)
{
	is_group_joined = true;

	k_sem_give(&wait_data);
}

static void group_left(struct net_mgmt_event_callback *cb,
			 uint32_t nm_event, struct net_if *iface)
{
	is_group_left = true;

	k_sem_give(&wait_data);
}

static struct mgmt_events {
	uint32_t event;
	net_mgmt_event_handler_t handler;
	struct net_mgmt_event_callback cb;
} mgmt_events[] = {
	{ .event = NET_EVENT_IPV6_MCAST_JOIN, .handler = group_joined },
	{ .event = NET_EVENT_IPV6_MCAST_LEAVE, .handler = group_left },
	{ 0 }
};

static void setup_mgmt_events(void)
{
	int i;

	for (i = 0; mgmt_events[i].event; i++) {
		net_mgmt_init_event_callback(&mgmt_events[i].cb,
					     mgmt_events[i].handler,
					     mgmt_events[i].event);

		net_mgmt_add_event_callback(&mgmt_events[i].cb);
	}
}

static void mld_setup(void)
{
	struct net_if_addr *ifaddr;

	setup_mgmt_events();

	iface = net_if_get_default();

	assert_not_null(iface, "Interface is NULL");

	ifaddr = net_if_ipv6_addr_add(iface, &my_addr,
				      NET_ADDR_MANUAL, 0);

	assert_not_null(ifaddr, "Cannot add IPv6 address");

	/* The semaphore is there to wait the data to be received. */
	k_sem_init(&wait_data, 0, UINT_MAX);
}

static void join_group(void)
{
	int ret;

	net_ipv6_addr_create(&mcast_addr, 0xff02, 0, 0, 0, 0, 0, 0, 0x0001);

	ret = net_ipv6_mld_join(iface, &mcast_addr);

	assert_equal(ret, 0, "Cannot join IPv6 multicast group");

	k_yield();
}

static void leave_group(void)
{
	int ret;

	net_ipv6_addr_create(&mcast_addr, 0xff02, 0, 0, 0, 0, 0, 0, 0x0001);

	ret = net_ipv6_mld_leave(iface, &mcast_addr);

	assert_equal(ret, 0, "Cannot leave IPv6 multicast group");

	k_yield();
}

static void catch_join_group(void)
{
	is_group_joined = false;

	join_group();

	if (k_sem_take(&wait_data, WAIT_TIME)) {
		assert_true(0, "Timeout while waiting join event");
	}

	if (!is_group_joined) {
		assert_true(0, "Did not catch join event");
	}

	is_group_joined = false;
}

static void catch_leave_group(void)
{
	is_group_joined = false;

	leave_group();

	if (k_sem_take(&wait_data, WAIT_TIME)) {
		assert_true(0, "Timeout while waiting leave event");
	}

	if (!is_group_left) {
		assert_true(0, "Did not catch leave event");
	}

	is_group_left = false;
}

static void verify_join_group(void)
{
	is_join_msg_ok = false;

	join_group();

	if (k_sem_take(&wait_data, WAIT_TIME)) {
		assert_true(0, "Timeout while waiting join event");
	}

	if (!is_join_msg_ok) {
		assert_true(0, "Join msg invalid");
	}

	is_join_msg_ok = false;
}

static void verify_leave_group(void)
{
	is_leave_msg_ok = false;

	leave_group();

	if (k_sem_take(&wait_data, WAIT_TIME)) {
		assert_true(0, "Timeout while waiting leave event");
	}

	if (!is_leave_msg_ok) {
		assert_true(0, "Leave msg invalid");
	}

	is_leave_msg_ok = false;
}

void test_main(void)
{
	ztest_test_suite(net_mld_test,
			 ztest_unit_test(mld_setup),
			 ztest_unit_test(join_group),
			 ztest_unit_test(leave_group),
			 ztest_unit_test(catch_join_group),
			 ztest_unit_test(catch_leave_group),
			 ztest_unit_test(verify_join_group),
			 ztest_unit_test(verify_leave_group)
			 );

	ztest_run_test_suite(net_mld_test);
}