/*
 * Copyright (C) 2000, 2001, 2002, 2003, 2004 Shawn Betts <sabetts@vcn.bc.ca>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA 02111-1307 USA.
 */

#include <ctype.h>
#include <errno.h>
#include <err.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

#include "poison.h"

static const char *
extract_shell_part(const char *p)
{
	if (strncmp(p, "exec", 4) && strncmp(p, "verbexec", 8))
		return NULL;
	while (*p && !isspace((unsigned char) *p))
		p++;
	while (*p && isspace((unsigned char) *p))
		p++;
	if (*p)
		return p;
	return NULL;
}

struct history_item {
	struct list_head node;
	char *line;
};

static struct history {
	struct list_head head, *current;
	size_t count;
}       histories[hist_COUNT];

static void
history_add_upto(int history_id, const char *item, size_t max)
{
	struct history *h = histories + history_id;
	struct history_item *i;

	if (item == NULL || *item == '\0' || isspace((unsigned char) *item))
		return;

	list_last(i, &histories[history_id].head, node);
	if (i && !strcmp(i->line, item))
		return;

	if (history_id == hist_COMMAND) {
		const char *p = extract_shell_part(item);
		if (p)
			history_add_upto(hist_SHELLCMD, p, max);
	}
	while (h->count >= max) {
		list_first(i, &h->head, node);
		if (!i) {
			h->count = 0;
			break;
		}
		list_del(&i->node);
		free(i->line);
		free(i);
		h->count--;
	}

	if (max == 0)
		return;

	i = xmalloc(sizeof(*i));
	i->line = xstrdup(item);

	list_add_tail(&i->node, &h->head);
	h->count++;
}

void
history_add(int history_id, const char *item)
{
	history_add_upto(history_id, item, 0);
}

void
history_load(void)
{
	int id;

	/* Initialize history structures but don't load from file */
	for (id = hist_NONE; id < hist_COUNT; id++) {
		INIT_LIST_HEAD(&histories[id].head);
		histories[id].current = &histories[id].head;
		histories[id].count = 0;
	}

	/* History is disabled - do not load from file */
}

void
history_save(void)
{
	/* History is disabled - do nothing */
	return;
}

void
history_reset(void)
{
	int id;

	for (id = hist_NONE; id < hist_COUNT; id++)
		histories[id].current = &histories[id].head;
}

const char *
history_previous(int history_id)
{
	if (history_id == hist_NONE)
		return NULL;
	/* return NULL, if list empty or already at first */
	if (histories[history_id].current == histories[history_id].head.next)
		return NULL;
	histories[history_id].current = histories[history_id].current->prev;
	return list_entry(histories[history_id].current,
	    struct history_item, node)->line;
}

const char *
history_next(int history_id)
{
	if (history_id == hist_NONE)
		return NULL;
	/* return NULL, if list empty or already behind last */
	if (histories[history_id].current == &histories[history_id].head)
		return NULL;
	histories[history_id].current = histories[history_id].current->next;
	if (histories[history_id].current == &histories[history_id].head)
		return NULL;
	return list_entry(histories[history_id].current, struct history_item,
	    node)->line;
}
