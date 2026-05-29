// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * Minimal telnet server for MediaTek web failsafe (RFC 854 compliant).
 *
 * Uses the mtk_tcp framework to accept telnet connections and provides
 * a U-Boot command-line interface.  Telnet option negotiation follows
 * RFC 854: the server sends WILL ECHO (server echoes characters),
 * WILL SGA (full-duplex, suppress go-ahead), and DO NAWS (request
 * window-size notifications).  Client negotiation requests are
 * properly responded to.
 */

#include <command.h>
#include <console.h>
#include <env.h>
#include <errno.h>
#include <malloc.h>
#include <membuf.h>
#include <net.h>
#include <net/mtk_tcp.h>
#include <net/mtk_telnetd.h>
#include <version_string.h>
#include <vsprintf.h>
#include <asm/global_data.h>

#ifdef CONFIG_MTK_DHCPD
#include <net/mtk_dhcpd.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

/**
 * is_network_command() - check if a command will call net_loop()
 *
 * Returns true for commands that perform network I/O through net_loop(),
 * which halts ethernet on completion.  The main poll loop needs to
 * reinitialize ethernet afterwards.
 */
static bool is_network_command(const char *cmd)
{
	return strstr(cmd, "tftp") || strstr(cmd, "ping") ||
	       strstr(cmd, "dhcp") || strstr(cmd, "bootp") ||
	       strstr(cmd, "nfs")  || strstr(cmd, "rarp") ||
	       strstr(cmd, "wget") || strstr(cmd, "tcp");
}

/* ------------------------------------------------------------------
 * Telnet protocol constants
 * ------------------------------------------------------------------ */

#define IAC		255	/* Interpret As Command		*/
#define WILL		251
#define WONT		252
#define DO		253
#define DONT		254
#define SB		250	/* Subnegotiation begin		*/
#define SE		240	/* Subnegotiation end		*/

#define TELOPT_ECHO	1
#define TELOPT_SGA	3	/* Suppress Go Ahead		*/
#define TELOPT_NAWS	31	/* Negotiate About Window Size	*/

#ifndef WEBUI_FAILSAFE_GIT_HASH
#define WEBUI_FAILSAFE_GIT_HASH	"unknown"
#endif

#ifndef WEBUI_FAILSAFE_GIT_DIRTY
#define WEBUI_FAILSAFE_GIT_DIRTY	0
#endif

/* ------------------------------------------------------------------
 * Buffer sizes
 * ------------------------------------------------------------------ */

#define TELNETD_INBUF_SIZE	2048	/* Raw TCP rx buffer		*/
#define TELNETD_OUTBUF_SIZE	8192	/* Max console output per cmd	*/
#define TELNETD_CMD_MAX		512	/* Max command line length	*/
#define TELNETD_EDIT_BUF_SIZE	512	/* Max accumulated edit responses */

/* ------------------------------------------------------------------
 * Session state
 * ------------------------------------------------------------------ */

enum telnetd_state {
	TELNETD_S_IDLE = 0,
	TELNETD_S_RESPONDING,
};

struct telnetd_pdata {
	enum telnetd_state state;

	char inbuf[TELNETD_INBUF_SIZE];
	u32 inbuf_size;

	char cmdbuf[TELNETD_CMD_MAX];
	u32 cmdlen;
	bool skip_lf;

	char *outbuf;		/* malloc'd output buffer	*/
	u32 outbuf_len;
	bool outbuf_pending;

	char edit_outbuf[TELNETD_EDIT_BUF_SIZE];
	u32 edit_outbuf_len;

	bool executing;		/* true while run_command() is active –
				 * prevents re-entrant telnetd_execute() calls
				 * triggered by mtk_tcp_periodic_check()
				 * running inside net_loop(). */
};

/* ------------------------------------------------------------------
 * Global instance
 * ------------------------------------------------------------------ */

static struct {
	u16 port;
	bool running;
} telnetd_inst;

static const char *telnetd_get_prompt(void);

/* ------------------------------------------------------------------
 * Negotiation sequence sent on every new connection.
 *
 *   WONT ECHO   – server will NOT echo; client does local echo
 *   WILL SGA    – full-duplex (suppress go-ahead)
 *   DO   NAWS   – please inform us of your window size
 *
 * Followed by a welcome banner that the telnet client displays after
 * the IAC escapes have been consumed.
 * ------------------------------------------------------------------ */

/*
 * Telnet option negotiation sequence (RFC 854).
 * Sent on every new connection to set desired options.
 *
 *   WILL ECHO   – server echoes characters (traditional line mode)
 *   WILL SGA    – full-duplex (suppress go-ahead)
 *   DO   NAWS   – please inform us of your window size
 */
static const char telnet_iac_negotiation[] = {
	IAC, WILL, TELOPT_ECHO,
	IAC, WILL, TELOPT_SGA,
	IAC, DO,   TELOPT_NAWS,
};

/*
 * Greeting text prefix (dynamic version info appended).
 */
static const char telnet_greeting_text[] = {
	'\r', '\n',
	'U', '-', 'B', 'o', 'o', 't', ' ',
	'T', 'e', 'l', 'n', 'e', 't', ' ',
	'C', 'o', 'n', 's', 'o', 'l', 'e',
	'\r', '\n',
};

static const char telnet_fallback_text[] = {
	'U', '-', 'B', 'o', 'o', 't', ' ',
	'T', 'e', 'l', 'n', 'e', 't', ' ',
	'C', 'o', 'n', 's', 'o', 'l', 'e',
	'\r', '\n',
	'A', 'u', 't', 'h', 'o', 'r', ':', ' ',
	'Y', 'u', 'z', 'h', 'i', 'i', '0', '7', '1', '8',
	'\r', '\n', '\r', '\n',
	'M', 'T', 'K', '>', ' ',
};

static size_t telnetd_build_greeting(char *buf, size_t buf_sz)
{
	const char *git_hash = WEBUI_FAILSAFE_GIT_HASH;
	const char *build_variant = NULL;
	const char *prompt = telnetd_get_prompt();
	bool dirty = !!WEBUI_FAILSAFE_GIT_DIRTY;
	size_t off = 0;
	int n;

	if (!buf || buf_sz < 64)
		return 0;

	if (!git_hash || !git_hash[0])
		git_hash = "unknown";

#ifdef CONFIG_WEBUI_FAILSAFE_BUILD_VARIANT
	build_variant = CONFIG_WEBUI_FAILSAFE_BUILD_VARIANT;
	if (!build_variant[0])
		build_variant = NULL;
#endif

	memcpy(buf + off, telnet_iac_negotiation,
	       sizeof(telnet_iac_negotiation));
	off += sizeof(telnet_iac_negotiation);

	memcpy(buf + off, telnet_greeting_text,
	       sizeof(telnet_greeting_text));
	off += sizeof(telnet_greeting_text);

	n = snprintf(buf + off, buf_sz - off,
		     "Version: %s\r\nGit Hash: %s%s\r\n%s%s%s\r\n",
                     version_string,
                     git_hash, dirty ? " (dirty)" : "",
                     build_variant ? "Build: " : "",
                     build_variant ? build_variant : "",
                     build_variant ? "\r\n" : "");
	if (n < 0 || (size_t)n >= buf_sz - off)
		return 0;
	off += n;

	n = snprintf(buf + off, buf_sz - off,
		     "Author: Yuzhii0718\r\n\r\n%s", prompt);
	if (n < 0 || (size_t)n >= buf_sz - off)
		return 0;
	off += n;

	return off;
}

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */

static const char *telnetd_get_prompt(void)
{
	const char *p = env_get("prompt");

	if (p && p[0])
		return p;

#ifdef CONFIG_SYS_PROMPT
	return CONFIG_SYS_PROMPT;
#else
	return "MTK> ";
#endif
}

static int telnetd_ensure_recording(void)
{
	int ret;

	if (!gd)
		return -ENODEV;

	if (!gd->console_out.start) {
		ret = console_record_init();
		if (ret)
			return ret;
	}

	gd->flags |= GD_FLG_RECORD;
	return 0;
}

static char *telnetd_normalize_output(const char *src, size_t len,
				      size_t *out_len)
{
	char *dst;
	size_t i, di = 0;
	bool last_was_cr = false;

	if (!src || !len)
		return NULL;

	dst = malloc(len * 2 + 1);
	if (!dst)
		return NULL;

	for (i = 0; i < len; i++) {
		unsigned char c = src[i];

		if (c == '\n') {
			if (!last_was_cr)
				dst[di++] = '\r';
			dst[di++] = '\n';
			last_was_cr = false;
			continue;
		}

		if (c == '\r')
			last_was_cr = true;
		else
			last_was_cr = false;

		dst[di++] = c;
	}

	if (out_len)
		*out_len = di;

	return dst;
}

static void telnetd_send_or_queue(struct mtk_tcp_cb_data *cbd,
				   struct telnetd_pdata *pdata,
				   char *buf, u32 len)
{
	int ret;

	if (!buf || !len)
		return;

	ret = mtk_tcp_send_data(cbd->conn, buf, len);
	if (ret) {
		pdata->outbuf = buf;
		pdata->outbuf_len = len;
		pdata->outbuf_pending = true;
		pdata->state = TELNETD_S_RESPONDING;
		return;
	}

	pdata->outbuf = buf;
	pdata->outbuf_len = len;
	pdata->outbuf_pending = false;
	pdata->state = TELNETD_S_RESPONDING;
}

/**
 * telnetd_iac_skip() – return the number of raw bytes to skip for an
 * IAC sequence that starts at @buf[0].
 *
 * On entry buf[0] == IAC and buf[1] is valid.
 *
 * Returns the total skip count (2, 3, or up to the end of a
 * sub-negotiation).  If the sequence is incomplete (e.g. IAC SB
 * without a terminating IAC SE within @buflen), returns 0 so the
 * caller can keep the tail for the next rx.
 */
static u32 telnetd_iac_skip(const char *buf, u32 buflen)
{
	unsigned char cmd = buf[1];

	/* IAC IAC – literal 0xff in the data stream */
	if (cmd == IAC)
		return 2;

	/* Sub-negotiation: IAC SB <opt> ... IAC SE */
	if (cmd == SB) {
		u32 pos = 2;

		while (pos + 1 < buflen) {
			if ((unsigned char)buf[pos] == IAC &&
			    (unsigned char)buf[pos + 1] == SE)
				return pos + 2;
			pos++;
		}
		/* incomplete – keep everything */
		return 0;
	}

	/* Two-byte commands: NOP(241), AYT(246), etc. (240-249) */
	if (cmd >= 240 && cmd <= 249)
		return 2;

	/*
	 * Three-byte negotiations: WILL / WONT / DO / DONT + option.
	 * Require all three bytes; if truncated, the caller keeps the
	 * tail for the next TCP segment.
	 */
	if ((cmd == WILL || cmd == WONT || cmd == DO || cmd == DONT)) {
		if (buflen >= 3)
			return 3;
		return 0;	/* incomplete – wait for more data */
	}

	/* Unknown – skip the two bytes we can identify */
	return 2;
}

/**
 * telnetd_process_iac() – handle a Telnet IAC negotiation sequence.
 *
 * @buf: pointer to IAC command (buf[0] == IAC, buf[1] == command)
 * @buflen: number of bytes available (must be >= 3 for WILL/WONT/DO/DONT)
 *
 * Generates appropriate responses per RFC 854 and appends them to
 * pdata->edit_outbuf for batched sending.
 */
static void telnetd_process_iac(struct telnetd_pdata *pdata,
				const char *buf, u32 buflen)
{
	unsigned char cmd = buf[1];
	unsigned char option = buf[2];

	if (buflen < 3)
		return;

	/* Only handle WILL/WONT/DO/DONT */
	if (cmd != WILL && cmd != WONT && cmd != DO && cmd != DONT)
		return;

	/*
	 * Response strategy per RFC 854:
	 * - DO X   → WILL X (if we support) or WONT X
	 * - DONT X → WONT X
	 * - WILL X → DO X   (if we want client to do) or DONT X
	 * - WONT X → DONT X
	 *
	 * Supported options:
	 *   ECHO: we send WILL (server echoes), client should DO ECHO
	 *   SGA:  we send WILL (full-duplex), client should DO SGA
	 *   NAWS: we send DO (request window size), client should WILL NAWS
	 */
	unsigned char response_cmd = 0;

	switch (cmd) {
	case DO:
		/* Client asks us to enable option */
		if (option == TELOPT_SGA || option == TELOPT_ECHO)
			response_cmd = WILL;  /* we support SGA and ECHO */
		else
			response_cmd = WONT;  /* refuse others */
		break;
	case DONT:
		/* Client insists we disable option */
		response_cmd = WONT;
		break;
	case WILL:
		/* Client wants to enable option */
		if (option == TELOPT_SGA || option == TELOPT_NAWS)
			response_cmd = DO;    /* agree */
		else
			response_cmd = DONT;  /* refuse */
		break;
	case WONT:
		/* Client refuses to enable option */
		response_cmd = DONT;
		break;
	}

	/* Append response to edit_outbuf if space permits */
	if (response_cmd && pdata->edit_outbuf_len + 3 <= TELNETD_EDIT_BUF_SIZE) {
		pdata->edit_outbuf[pdata->edit_outbuf_len++] = IAC;
		pdata->edit_outbuf[pdata->edit_outbuf_len++] = response_cmd;
		pdata->edit_outbuf[pdata->edit_outbuf_len++] = option;
	}
}

/**
 * telnetd_edit_backspace() – append a backspace-erase sequence to edit_outbuf.
 *
 * Erases one character on the client screen by sending "\b \b".
 * Used in WILL ECHO mode where the server controls the display.
 */
static void telnetd_edit_backspace(struct telnetd_pdata *pdata)
{
	if (pdata->edit_outbuf_len + 3 <= TELNETD_EDIT_BUF_SIZE) {
		pdata->edit_outbuf[pdata->edit_outbuf_len++] = '\b';
		pdata->edit_outbuf[pdata->edit_outbuf_len++] = ' ';
		pdata->edit_outbuf[pdata->edit_outbuf_len++] = '\b';
	}
}

/**
 * telnetd_send_prompt() – send a prompt with preceding CRLF.
 *
 * Constructs "\r\n" + prompt and sends it via telnetd_send_or_queue().
 */
static void telnetd_send_prompt(struct mtk_tcp_cb_data *cbd,
				struct telnetd_pdata *pdata,
				const char *prompt)
{
	size_t plen = strlen(prompt);
	char *buf = malloc(plen + 3);

	if (buf) {
		buf[0] = '\r';
		buf[1] = '\n';
		memcpy(buf + 2, prompt, plen);
		buf[2 + plen] = '\0';
		telnetd_send_or_queue(cbd, pdata, buf, plen + 2);
	}
}

/* ------------------------------------------------------------------
 * Command execution
 * ------------------------------------------------------------------ */

static void telnetd_execute(struct mtk_tcp_cb_data *cbd,
			    const char *cmd)
{
	struct telnetd_pdata *pdata = cbd->pdata;
	const char *prompt = telnetd_get_prompt();
	char *outbuf;
	int avail;
	struct membuf saved_console_out;
	struct membuf telnet_console_out;
	char *raw_out = NULL;
	bool use_private_console_out = false;

	/* Empty command -> just re-print the prompt */
	if (!cmd[0]) {
		telnetd_send_prompt(cbd, pdata, prompt);
		return;
	}

	/* Ensure console output is being recorded */
	if (telnetd_ensure_recording()) {
		outbuf = malloc(64);
		if (outbuf) {
			snprintf(outbuf, 64,
				 "Error: console recording unavailable\r\n");
			telnetd_send_or_queue(cbd, pdata, outbuf, strlen(outbuf));
		}
		return;
	}

	saved_console_out = gd->console_out;
	if (!membuf_new(&telnet_console_out, TELNETD_OUTBUF_SIZE)) {
		gd->console_out = telnet_console_out;
		use_private_console_out = true;
	}

	/* Reset record so we only capture output from this command */
	console_record_reset();

	/* Detect network commands before running – they may halt ethernet */
	bool was_network_cmd = is_network_command(cmd);

	/*
	 * Mark that we are executing a command.  This prevents re-entrant
	 * telnetd_execute() calls that can happen when net_loop() runs
	 * mtk_tcp_periodic_check() for all protocols (our non-blocking
	 * failsafe modification).  Without this guard, the callback for
	 * this same telnet connection could fire again and corrupt the
	 * saved gd->console_out / membuf state.
	 */
	pdata->executing = true;

	/* Run the U-Boot command */
	run_command(cmd, 0);

	pdata->executing = false;

	/*
	 * If a network command was executed, net_loop() inside it called
	 * eth_halt() on completion.  We must reinitialize the ethernet
	 * device RIGHT NOW so that mtk_tcp_send_data() can deliver the
	 * captured output to the telnet client.  We also re-register the
	 * DHCP handler that net_clear_handlers() removed.
	 */
	if (was_network_cmd) {
		eth_init();
#ifdef CONFIG_MTK_DHCPD
		if (mtk_dhcpd_is_running())
			mtk_dhcpd_start();
#endif
	}

	/* Print a fresh prompt after the command's output */
	if (prompt[0] != '\n')
		printf("\n%s", prompt);
	else
		printf("%s", prompt);

	/*
	 * Read captured console output and always send response.
	 *
	 * Prepend "\r\n" so the output starts on a new line after the
	 * echoed command.  Without this, the first line of output would
	 * be appended to the command line, e.g.:
	 *   "MT7981> ubi list0: kernel"   (wrong)
	 * instead of:
	 *   "MT7981> ubi list\r\n0: kernel" (correct)
	 */
	avail = membuf_avail(&gd->console_out);
	if (avail > TELNETD_OUTBUF_SIZE)
		avail = TELNETD_OUTBUF_SIZE;

	if (avail > 0) {
		size_t norm_len = 0;
		int got;

		raw_out = malloc(avail + 2);
		if (raw_out) {
			/* Prepend \r\n for the line break after user input */
			raw_out[0] = '\r';
			raw_out[1] = '\n';
			got = membuf_get(&gd->console_out, raw_out + 2, avail);
			outbuf = telnetd_normalize_output(raw_out, got + 2,
							  &norm_len);
			if (outbuf) {
				free(raw_out);
				raw_out = NULL;
				telnetd_send_or_queue(cbd, pdata, outbuf, norm_len);
			} else {
				telnetd_send_or_queue(cbd, pdata, raw_out, got + 2);
				raw_out = NULL;
			}
		}
	} else {
		/* No output: send prompt to indicate command completed */
		telnetd_send_prompt(cbd, pdata, prompt);
	}

	if (use_private_console_out) {
		membuf_dispose(&gd->console_out);
		gd->console_out = saved_console_out;
	}

	free(raw_out);
}

/* ------------------------------------------------------------------
 * Input processing
 * ------------------------------------------------------------------ */

/**
 * telnetd_flush_edit_outbuf() – send accumulated edit responses
 * (echoed characters, backspace erasures, ^C, new prompts, IAC
 * negotiation replies) to the client in a single TCP segment
 * and reset the accumulator.
 */
static void telnetd_flush_edit_outbuf(struct mtk_tcp_cb_data *cbd,
				      struct telnetd_pdata *pdata)
{
	if (!pdata->edit_outbuf_len)
		return;

	if (!mtk_tcp_send_data(cbd->conn, pdata->edit_outbuf,
			       pdata->edit_outbuf_len))
		pdata->edit_outbuf_len = 0;
}

/**
 * telnetd_process_input() – strip telnet IAC escapes from the buffered
 * raw input and execute commands on every complete line (delimited by
 * CR-NUL, CR-LF, bare CR, or bare LF).
 */
static void telnetd_process_input(struct mtk_tcp_cb_data *cbd)
{
	struct telnetd_pdata *pdata = cbd->pdata;
	u32 i;

	/* Walk through the raw buffer and consume bytes */
	i = 0;

	while (i < pdata->inbuf_size) {
		unsigned char c = pdata->inbuf[i];

		if (pdata->skip_lf) {
			pdata->skip_lf = false;
			if (c == '\0' || c == '\n') {
				i++;
				continue;
			}
		}

		/* ---- Telnet IAC escape ---- */
		if (c == IAC) {
			u32 skip;

			if (i + 1 >= pdata->inbuf_size) {
				/*
				 * Incomplete IAC at the end of the buffer –
				 * keep it for the next rx.
				 */
				break;
			}

			skip = telnetd_iac_skip(&pdata->inbuf[i],
					       pdata->inbuf_size - i);
			if (!skip) {
				/* Incomplete sub-negotiation */
				break;
			}

			/* IAC IAC is a literal 0xff – pass it through */
			if ((unsigned char)pdata->inbuf[i + 1] == IAC) {
				if (pdata->cmdlen < TELNETD_CMD_MAX - 1)
					pdata->cmdbuf[pdata->cmdlen++] = IAC;
				i += skip;
				continue;
			}

			/* Handle negotiation sequences per RFC 854 */
			telnetd_process_iac(pdata, &pdata->inbuf[i], skip);
			i += skip;
			continue;
		}

		/* ---- Line terminators ---- */
		if (c == '\r' || c == '\n') {
			pdata->cmdbuf[pdata->cmdlen] = '\0';
			i++;
			if (c == '\r')
				pdata->skip_lf = true;
			telnetd_execute(cbd, pdata->cmdbuf);
			pdata->cmdlen = 0;
			pdata->cmdbuf[0] = '\0';
			/* Stop if we entered RESPONDING */
			if (pdata->state != TELNETD_S_IDLE)
				break;
			continue;
		}

		/* ---- ANSI escape sequences (arrow keys, etc.) ---- */
		if (c == '\x1b') {
			if (i + 1 < pdata->inbuf_size &&
			    pdata->inbuf[i + 1] == '[') {
				/* CSI: ESC [ ... terminator (0x40-0x7E) */
				u32 j = i + 2;

				while (j < pdata->inbuf_size) {
					unsigned char t = pdata->inbuf[j];

					if (t >= 0x40 && t <= 0x7e) {
						j++;
						break; /* found terminator */
					}
					if (t < 0x20 || t > 0x2f)
						break; /* malformed */
					j++;
				}
				if (j > i + 2) {
					i = j;
					continue;
				}
				/* Incomplete — keep for next rx */
				break;
			}
			/* Lone ESC or unknown escape — skip it */
			i++;
			continue;
		}

		/* ---- Backspace / DEL ---- */
		if (c == '\b' || c == 0x7f) {
			if (pdata->cmdlen > 0) {
				pdata->cmdlen--;
				telnetd_edit_backspace(pdata);
			}
			i++;
			continue;
		}

		/* ---- Control characters ---- */
		if (c == '\x03') {
			/* Ctrl+C — clear line, print ^C + new prompt */
			const char *prompt = telnetd_get_prompt();
			u32 plen = strlen(prompt);
			u32 need = 6 + plen;

			pdata->cmdlen = 0;
			pdata->cmdbuf[0] = '\0';
			if (pdata->edit_outbuf_len + need <=
			    TELNETD_EDIT_BUF_SIZE) {
				pdata->edit_outbuf[
				  pdata->edit_outbuf_len++] = '^';
				pdata->edit_outbuf[
				  pdata->edit_outbuf_len++] = 'C';
				pdata->edit_outbuf[
				  pdata->edit_outbuf_len++] = '\r';
				pdata->edit_outbuf[
				  pdata->edit_outbuf_len++] = '\n';
				memcpy(pdata->edit_outbuf +
				       pdata->edit_outbuf_len,
				       prompt, plen);
				pdata->edit_outbuf_len += plen;
			}
			i++;
			continue;
		}

		if (c == '\x15') {
			/* Ctrl+U — clear entire line */
			while (pdata->cmdlen > 0 &&
			       pdata->edit_outbuf_len + 3 <=
			       TELNETD_EDIT_BUF_SIZE) {
				pdata->cmdlen--;
				telnetd_edit_backspace(pdata);
			}
			pdata->cmdlen = 0;
			pdata->cmdbuf[0] = '\0';
			i++;
			continue;
		}

		if (c == '\x17') {
			/* Ctrl+W — delete previous word */
			while (pdata->cmdlen > 0 &&
			       pdata->cmdbuf[pdata->cmdlen - 1] == ' ') {
				if (pdata->edit_outbuf_len + 3 >
				    TELNETD_EDIT_BUF_SIZE)
					break;
				pdata->cmdlen--;
				telnetd_edit_backspace(pdata);
			}
			while (pdata->cmdlen > 0 &&
			       pdata->cmdbuf[pdata->cmdlen - 1] != ' ') {
				if (pdata->edit_outbuf_len + 3 >
				    TELNETD_EDIT_BUF_SIZE)
					break;
				pdata->cmdlen--;
				telnetd_edit_backspace(pdata);
			}
			i++;
			continue;
		}

		/* ---- TAB — no tab-completion in U-Boot ---- */
		if (c == '\t') {
			i++;
			continue;
		}

		if (c < 0x20) {
			/* Other control chars (excluding handled above) */
			i++;
			continue;
		}

		/* ---- Regular character ---- */
		if (pdata->cmdlen < TELNETD_CMD_MAX - 1) {
			pdata->cmdbuf[pdata->cmdlen++] = c;
			/* Echo back to client (WILL ECHO mode) */
			if (pdata->edit_outbuf_len < TELNETD_EDIT_BUF_SIZE)
				pdata->edit_outbuf[pdata->edit_outbuf_len++] = c;
		}
		i++;
	}

	/* Flush accumulated edit responses (backspace erasures, etc.) */
	telnetd_flush_edit_outbuf(cbd, pdata);

	/* Remove consumed bytes from the raw buffer */
	if (i > 0) {
		u32 remaining = pdata->inbuf_size - i;

		if (remaining > 0)
			memmove(pdata->inbuf, pdata->inbuf + i, remaining);
		pdata->inbuf_size = remaining;
		pdata->inbuf[remaining] = '\0';
	}
}

/* ------------------------------------------------------------------
 * TCP callback
 * ------------------------------------------------------------------ */

static void telnetd_callback(struct mtk_tcp_cb_data *cbd)
{
	struct telnetd_pdata *pdata;
	u8 sip[4];

	switch (cbd->status) {
	case MTK_TCP_CB_NEW_CONN:
		pdata = calloc(1, sizeof(*pdata));
		if (!pdata) {
			mtk_tcp_close_conn(cbd->conn, 1);
			break;
		}

		cbd->pdata = pdata;
		mtk_tcp_conn_set_pdata(cbd->conn, pdata);

		memcpy(sip, &cbd->sip, 4);
		printf("Telnet connection from %d.%d.%d.%d:%d\n",
		       sip[0], sip[1], sip[2], sip[3], ntohs(cbd->sp));

		/*
		 * Send negotiations + welcome banner as a single buffer.
		 * Prefer a dynamic banner so we can include version info;
		 * fall back to the static banner if allocation fails.
		 */
		{
			char *greeting = malloc(512);
			size_t greeting_len = 0;
			char *fallback = NULL;

			if (greeting) {
				greeting_len = telnetd_build_greeting(greeting, 512);
				if (greeting_len) {
					telnetd_send_or_queue(cbd, pdata, greeting,
							      greeting_len);
					break;
				}

				free(greeting);
			}

			{
				size_t nego_sz = sizeof(telnet_iac_negotiation);
				size_t text_sz = sizeof(telnet_fallback_text);

				fallback = malloc(nego_sz + text_sz);
				if (fallback) {
					memcpy(fallback, telnet_iac_negotiation, nego_sz);
					memcpy(fallback + nego_sz, telnet_fallback_text, text_sz);
					telnetd_send_or_queue(cbd, pdata, fallback,
							      nego_sz + text_sz);
				} else {
					/* Low memory: send static negotiation + text */
					if (!mtk_tcp_send_data(cbd->conn,
							      telnet_iac_negotiation,
							      sizeof(telnet_iac_negotiation))) {
						if (!mtk_tcp_send_data(cbd->conn,
								      telnet_fallback_text,
								      sizeof(telnet_fallback_text))) {
							pdata->outbuf = NULL;
							pdata->outbuf_len = 0;
							pdata->outbuf_pending = false;
							pdata->state = TELNETD_S_RESPONDING;
						}
					}
				}
			}
		}
		break;

	case MTK_TCP_CB_DATA_RCVD:
		pdata = cbd->pdata;
		if (!pdata)
			break;

		if (cbd->datalen) {
			/* Always buffer incoming data, even when not IDLE */
			u32 space = TELNETD_INBUF_SIZE -
				    pdata->inbuf_size - 1;
			u32 to_copy = min_t(u32, cbd->datalen, space);

			memcpy(pdata->inbuf + pdata->inbuf_size,
			       cbd->data, to_copy);
			pdata->inbuf_size += to_copy;
			pdata->inbuf[pdata->inbuf_size] = '\0';
			cbd->datalen = 0; /* consumed */
		}

		/*
		 * Skip processing if we are inside telnetd_execute() –
		 * mtk_tcp_periodic_check() runs from within net_loop()
		 * and could re-enter us while a command (e.g. tftpboot)
		 * is still executing.  The buffered data will be picked
		 * up when we return to IDLE after the command finishes.
		 */
		if (pdata->state == TELNETD_S_IDLE && !pdata->executing)
			telnetd_process_input(cbd);
		break;

	case MTK_TCP_CB_DATA_SENT:
		pdata = cbd->pdata;
		if (!pdata)
			break;

		if (pdata->state == TELNETD_S_RESPONDING) {
			if (pdata->outbuf_pending) {
				if (!mtk_tcp_send_data(cbd->conn, pdata->outbuf,
						      pdata->outbuf_len)) {
					pdata->outbuf_pending = false;
					return;
				}
				pdata->outbuf_pending = false;
			}

			/* Output buffer sent – free it, go idle */
			free(pdata->outbuf);
			pdata->outbuf = NULL;
			pdata->outbuf_len = 0;
			pdata->state = TELNETD_S_IDLE;

			/*
			 * Process any buffered input that arrived while
			 * we were busy sending the previous response.
			 * Skip if we are re-entered from net_loop().
			 */
			if (pdata->inbuf_size > 0 && !pdata->executing)
				telnetd_process_input(cbd);
		}
		break;

	case MTK_TCP_CB_REMOTE_CLOSED:
	case MTK_TCP_CB_CLOSED:
		pdata = cbd->pdata;
		if (pdata) {
			free(pdata->outbuf);
			free(pdata);
		}

		memcpy(sip, &cbd->sip, 4);
		printf("Telnet connection closed %d.%d.%d.%d:%d\n",
		       sip[0], sip[1], sip[2], sip[3], ntohs(cbd->sp));
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

int mtk_telnetd_start(u16 port)
{
	if (telnetd_inst.running)
		return -EALREADY;

	if (mtk_tcp_listen(htons(port), telnetd_callback))
		return -EIO;

	telnetd_inst.port = port;
	telnetd_inst.running = true;

	printf("Telnet server started on port %d\n", port);
	return 0;
}

void mtk_telnetd_stop(void)
{
	if (!telnetd_inst.running)
		return;

	mtk_tcp_listen_stop(htons(telnetd_inst.port));
	telnetd_inst.running = false;

	printf("Telnet server stopped\n");
}

bool mtk_telnetd_is_running(void)
{
	return telnetd_inst.running;
}

static int do_telnetd(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	if (argc < 2)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "start")) {
		u16 port = 23;

		if (argc > 2) {
			unsigned long p;

			p = simple_strtoul(argv[2], NULL, 10);
			if (p >= 1 && p <= 65535)
				port = (u16)p;
		} else {
			const char *env_port = env_get("telnet_port");

			if (env_port) {
				unsigned long p;

				p = simple_strtoul(env_port, NULL, 10);
				if (p >= 1 && p <= 65535)
					port = (u16)p;
			}
		}

		if (mtk_telnetd_start(port))
			printf("Failed to start telnet server\n");

		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "stop")) {
		mtk_telnetd_stop();
		return CMD_RET_SUCCESS;
	}

	return CMD_RET_USAGE;
}

U_BOOT_CMD(telnetd, 3, 0, do_telnetd,
	"Control telnet server",
	"start [port] - start telnet server (default port 23, or $telnet_port)\n"
	"telnetd stop - stop telnet server\n\n"
	"Environment:\n"
	"  telnet_port - default port for telnetd (if not specified on command line)\n"
	"  telnetd_enable - if set to a nonempty value, telnetd will start automatically on failsafe entry\n"
	"			set 0/false/no/off to disable automatic start on failsafe entry"
);
