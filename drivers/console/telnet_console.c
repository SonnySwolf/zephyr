/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Telnet console
 *
 *
 * Telnet console driver.
 * Hooks into the printk and fputc (for printf) modules.
 *
 * Telnet has been standardised in 1983
 * RFC 854 - https://tools.ietf.org/html/rfc854
 */

#define SYS_LOG_LEVEL CONFIG_SYS_LOG_TELNET_CONSOLE_LEVEL
#define SYS_LOG_DOMAIN "net/telnet"
#include <logging/sys_log.h>

#include <zephyr.h>
#include <init.h>
#include <misc/printk.h>

#include <console/console.h>
#include <net/buf.h>
#include <net/nbuf.h>
#include <net/net_ip.h>
#include <net/net_context.h>

/* Various definitions mapping the telnet service configuration options */
#define TELNET_PORT		CONFIG_TELNET_CONSOLE_PORT
#define TELNET_STACK_SIZE	CONFIG_TELNET_CONSOLE_THREAD_STACK
#define TELNET_PRIORITY		CONFIG_TELNET_CONSOLE_PRIO
#define TELNET_LINES		CONFIG_TELNET_CONSOLE_LINE_BUF_NUMBERS
#define TELNET_LINE_SIZE	CONFIG_TELNET_CONSOLE_LINE_BUF_SIZE
#define TELNET_TIMEOUT		K_MSEC(CONFIG_TELNET_CONSOLE_SEND_TIMEOUT)
#define TELNET_THRESHOLD	CONFIG_TELNET_CONSOLE_SEND_THRESHOLD

#define TELNET_MIN_MSG		2

/* These 2 structures below are used to store the console output
 * before sending it to the client. This is done to keep some
 * reactivity: the ring buffer is non-protected, if first line has
 * not been sent yet, and if next line is reaching the same index in rb,
 * the first one will be replaced. In a perfect world, this should
 * not happen. However on a loaded system with a lot of debug output
 * this is bound to happen eventualy, moreover if it does not have
 * the luxury to bufferize as much as it wants to. Just raise
 * CONFIG_TELNET_CONSOLE_LINE_BUF_NUMBERS if possible.
 */
struct line_buf {
	char buf[TELNET_LINE_SIZE];
	uint16_t len;
};

struct line_buf_rb {
	struct line_buf l_bufs[TELNET_LINES];
	uint16_t line_in;
	uint16_t line_out;
};

static struct line_buf_rb telnet_rb;

static char __noinit __stack telnet_stack[TELNET_STACK_SIZE];
static K_SEM_DEFINE(send_lock, 0, UINT_MAX);

/* The timer is used to send non-lf terminated output that has
 * been around for "tool long". This will prove to be useful
 * to send the shell prompt for instance.
 * ToDo: raise the time, incrementaly, when no output is coming
 *       so the timer will kick in less and less.
 */
static void telnet_send_prematurely(struct k_timer *timer);
static K_TIMER_DEFINE(send_timer, telnet_send_prematurely, NULL);

/* For now we handle a unique telnet client connection */
static struct net_context *client_cnx;
static struct net_buf *out_buf;
static int (*orig_printk_hook)(int);

static struct k_fifo *avail_queue;
static struct k_fifo *input_queue;

extern void __printk_hook_install(int (*fn)(int));
extern void *__printk_get_hook(void);

static void telnet_rb_init(void)
{
	int i;

	telnet_rb.line_in = 0;
	telnet_rb.line_out = 0;

	for (i = 0; i < TELNET_LINES; i++) {
		telnet_rb.l_bufs[i].len = 0;
	}
}

static void telnet_end_client_connection(void)
{
	__printk_hook_install(orig_printk_hook);
	orig_printk_hook = NULL;

	k_timer_stop(&send_timer);

	net_context_put(client_cnx);
	client_cnx = NULL;

	if (out_buf) {
		net_buf_unref(out_buf);
	}

	telnet_rb_init();
}

static int telnet_setup_out_buf(struct net_context *client)
{
	out_buf = net_nbuf_get_tx(client);
	if (!out_buf) {
		/* Cannot happen atm, nbuf waits indefinitely */
		return -ENOBUFS;
	}

	return 0;
}

static void telnet_rb_switch(void)
{
	telnet_rb.line_in++;

	if (telnet_rb.line_in == TELNET_LINES) {
		telnet_rb.line_in = 0;
	}

	telnet_rb.l_bufs[telnet_rb.line_in].len = 0;

	/* Unfortunately, we don't have enough line buffer,
	 * so we eat the next to be sent.
	 */
	if (telnet_rb.line_in == telnet_rb.line_out) {
		telnet_rb.line_out++;
		if (telnet_rb.line_out == TELNET_LINES) {
			telnet_rb.line_out = 0;
		}
	}

	k_timer_start(&send_timer, TELNET_TIMEOUT, TELNET_TIMEOUT);
	k_sem_give(&send_lock);
}

static inline struct line_buf *telnet_rb_get_line_out(void)
{
	uint16_t out = telnet_rb.line_out;

	telnet_rb.line_out++;
	if (telnet_rb.line_out == TELNET_LINES) {
		telnet_rb.line_out = 0;
	}

	if (!telnet_rb.l_bufs[out].len) {
		return NULL;
	}

	return &telnet_rb.l_bufs[out];
}

static inline struct line_buf *telnet_rb_get_line_in(void)
{
	return &telnet_rb.l_bufs[telnet_rb.line_in];
}

/* The actual printk hook */
static int telnet_console_out(int c)
{
	int key = irq_lock();
	struct line_buf *lb = telnet_rb_get_line_in();
	bool yield = false;

	lb->buf[lb->len++] = (char)c;

	if (c == '\n' || lb->len == TELNET_LINE_SIZE - 1) {
		lb->buf[lb->len-1] = '\r';
		lb->buf[lb->len++] = '\n';
		telnet_rb_switch();
		yield = true;
	}

	irq_unlock(key);

#ifdef CONFIG_TELNET_CONSOLE_DEBUG_DEEP
	/* This is ugly, but if one wants to debug telnet, it
	 * will also output the character to original console
	 */
	orig_printk_hook(c);
#endif

	if (yield) {
		k_yield();
	}

	return c;
}

static void telnet_send_prematurely(struct k_timer *timer)
{
	struct line_buf *lb = telnet_rb_get_line_in();

	if (lb->len >= TELNET_THRESHOLD) {
		telnet_rb_switch();
	}
}

static void telnet_sent_cb(struct net_context *client,
			   int status, void *token, void *user_data)
{
	if (status) {
		telnet_end_client_connection();
		SYS_LOG_ERR("Could not sent last buffer");
	}
}

static inline bool telnet_send(void)
{
	struct line_buf *lb = telnet_rb_get_line_out();

	if (lb) {
		net_nbuf_append(out_buf, lb->len, lb->buf);

		/* We reinitialize the line buffer */
		lb->len = 0;

		if (net_context_send(out_buf, telnet_sent_cb,
				     K_NO_WAIT, NULL, NULL) ||
		    telnet_setup_out_buf(client_cnx)) {
			return false;
		}
	}

	return true;
}

static inline void telnet_handle_input(struct net_buf *buf)
{
	struct console_input *input;
	uint16_t len, offset, pos;
	uint8_t *l_start;

	len = net_nbuf_appdatalen(buf);
	if (len > CONSOLE_MAX_LINE_LEN || len < TELNET_MIN_MSG) {
		return;
	}

	/* Telnet commands are ignored for now
	 * These are recognized by matching the IAC byte
	 * (IAC: Intepret As Command) which value is 255
	 */
	l_start = net_nbuf_appdata(buf);
	if (*l_start == 255) {
		return;
	}

	if (!avail_queue || !input_queue) {
		return;
	}

	input = k_fifo_get(avail_queue, K_NO_WAIT);
	if (!input) {
		return;
	}

	offset = net_buf_frags_len(buf) - len;
	net_nbuf_read(buf->frags, offset, &pos, len, input->line);

	/* LF/CR will be removed if only the line is not NUL terminated */
	if (input->line[len-1] != '\0') {
		if (input->line[len-1] == '\n') {
			input->line[len-1] = '\0';
		}

		if (input->line[len-2] == '\r') {
			input->line[len-2] = '\0';
		}
	}

	k_fifo_put(input_queue, input);
}

static void telnet_recv(struct net_context *client,
			struct net_buf *buf,
			int status,
			void *user_data)
{
	if (!buf || status) {
		telnet_end_client_connection();

		SYS_LOG_DBG("Telnet client dropped (AF_INET%s) status %d",
			    net_context_get_family(client) == AF_INET ?
			    "" : "6", status);
		return;
	}

	telnet_handle_input(buf);

	net_buf_unref(buf);
}

/* Telnet server loop, used to send buffered output in the RB */
static void telnet_run(void)
{
	while (true) {
		k_sem_take(&send_lock, K_FOREVER);

		if (!telnet_send()) {
			telnet_end_client_connection();
		}
	}
}

static void telnet_accept(struct net_context *client,
			  struct sockaddr *addr,
			  socklen_t addrlen,
			  int error,
			  void *user_data)
{
	if (error) {
		SYS_LOG_ERR("Error %d", error);
		goto error;
	}

	if (client_cnx) {
		SYS_LOG_WRN("A telnet client is already in.");
		goto error;
	}

	if (net_context_recv(client, telnet_recv, 0, NULL)) {
		SYS_LOG_ERR("Unable to setup reception (family %u)",
			    net_context_get_family(client));
		goto error;
	}

	if (telnet_setup_out_buf(client)) {
		goto error;
	}

	SYS_LOG_DBG("Telnet client connected (family AF_INET%s)",
		    net_context_get_family(client) == AF_INET ? "" : "6");

	orig_printk_hook = __printk_get_hook();
	__printk_hook_install(telnet_console_out);

	client_cnx = client;
	k_timer_start(&send_timer, TELNET_TIMEOUT, TELNET_TIMEOUT);

	return;
error:
	net_context_put(client);
}

static void telnet_setup_server(struct net_context **ctx, sa_family_t family,
				struct sockaddr *addr, socklen_t addrlen)
{
	if (net_context_get(family, SOCK_STREAM, IPPROTO_TCP, ctx)) {
		SYS_LOG_ERR("No context available");
		goto error;
	}

	if (net_context_bind(*ctx, addr, addrlen)) {
		SYS_LOG_ERR("Cannot bind on family AF_INET%s",
			    family == AF_INET ? "" : "6");
		goto error;
	}

	if (net_context_listen(*ctx, 0)) {
		SYS_LOG_ERR("Cannot listen on");
		goto error;
	}

	if (net_context_accept(*ctx, telnet_accept, 0, NULL)) {
		SYS_LOG_ERR("Cannot accept");
		goto error;
	}

	SYS_LOG_DBG("Telnet console enabled on AF_INET%s",
		    family == AF_INET ? "" : "6");

	return;
error:
	SYS_LOG_ERR("Unable to start telnet on AF_INET%s",
		    family == AF_INET ? "" : "6");

	if (*ctx) {
		net_context_put(*ctx);
		*ctx = NULL;
	}
}

void telnet_register_input(struct k_fifo *avail, struct k_fifo *lines,
			   uint8_t (*completion)(char *str, uint8_t len))
{
	ARG_UNUSED(completion);

	avail_queue = avail;
	input_queue = lines;
}

static int telnet_console_init(struct device *arg)
{
#ifdef CONFIG_NET_IPV4
	struct sockaddr_in any_addr4 = {
		.sin_family = AF_INET,
		.sin_port = htons(TELNET_PORT),
		.sin_addr = INADDR_ANY_INIT
	};
	static struct net_context *ctx4;
#endif
#ifdef CONFIG_NET_IPV6
	struct sockaddr_in6 any_addr6 = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(TELNET_PORT),
		.sin6_addr = IN6ADDR_ANY_INIT
	};
	static struct net_context *ctx6;
#endif

#ifdef CONFIG_NET_IPV4
	telnet_setup_server(&ctx4, AF_INET,
			    (struct sockaddr *)&any_addr4,
			    sizeof(any_addr4));
#endif
#ifdef CONFIG_NET_IPV6
	telnet_setup_server(&ctx6, AF_INET6,
			    (struct sockaddr *)&any_addr6,
			    sizeof(any_addr6));
#endif

	k_thread_spawn(&telnet_stack[0],
		       TELNET_STACK_SIZE,
		       (k_thread_entry_t)telnet_run,
		       NULL, NULL, NULL,
		       K_PRIO_COOP(TELNET_PRIORITY), 0, K_MSEC(10));

	SYS_LOG_INF("Telnet console initialized");

	return 0;
}

/* Telnet is initialized as an application directly, as it requires
 * the whole network stack to be ready.
 */
SYS_INIT(telnet_console_init, APPLICATION, CONFIG_TELNET_CONSOLE_INIT_PRIORITY);
