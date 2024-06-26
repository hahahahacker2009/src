/* $OpenBSD: cmd-paste-buffer.c,v 1.42 2024/02/13 08:03:50 nicm Exp $ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>
#include <vis.h>

#include "tmux.h"

/*
 * Paste paste buffer if present.
 */

static enum cmd_retval	cmd_paste_buffer_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_paste_buffer_entry = {
	.name = "paste-buffer",
	.alias = "pasteb",

	.args = { "db:prs:t:", 0, 0, NULL },
	.usage = "[-dpr] [-s separator] " CMD_BUFFER_USAGE " "
		 CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_paste_buffer_exec
};

static enum cmd_retval
cmd_paste_buffer_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window_pane	*wp = target->wp;
	struct paste_buffer	*pb;
	const char		*sepstr, *bufname, *bufdata, *bufend, *line;
	size_t			 seplen, bufsize;
	int			 bracket = args_has(args, 'p');

	if (window_pane_exited(wp)) {
		cmdq_error(item, "target pane has exited");
		return (CMD_RETURN_ERROR);
	}

	bufname = NULL;
	if (args_has(args, 'b'))
		bufname = args_get(args, 'b');

	if (bufname == NULL)
		pb = paste_get_top(NULL);
	else {
		pb = paste_get_name(bufname);
		if (pb == NULL) {
			cmdq_error(item, "no buffer %s", bufname);
			return (CMD_RETURN_ERROR);
		}
	}

	if (pb != NULL && ~wp->flags & PANE_INPUTOFF) {
		sepstr = args_get(args, 's');
		if (sepstr == NULL) {
			if (args_has(args, 'r'))
				sepstr = "\n";
			else
				sepstr = "\r";
		}
		seplen = strlen(sepstr);

		if (bracket && (wp->screen->mode & MODE_BRACKETPASTE))
			bufferevent_write(wp->event, "\033[200~", 6);

		bufdata = paste_buffer_data(pb, &bufsize);
		bufend = bufdata + bufsize;

		for (;;) {
			line = memchr(bufdata, '\n', bufend - bufdata);
			if (line == NULL)
				break;

			bufferevent_write(wp->event, bufdata, line - bufdata);
			bufferevent_write(wp->event, sepstr, seplen);

			bufdata = line + 1;
		}
		if (bufdata != bufend)
			bufferevent_write(wp->event, bufdata, bufend - bufdata);

		if (bracket && (wp->screen->mode & MODE_BRACKETPASTE))
			bufferevent_write(wp->event, "\033[201~", 6);
	}

	if (pb != NULL && args_has(args, 'd'))
		paste_free(pb);

	return (CMD_RETURN_NORMAL);
}
