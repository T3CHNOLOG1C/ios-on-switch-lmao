/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* IOSwitch TegraBPMP shim: the doubly linked list subset mailman.c uses (include/linux/list.h semantics). */
#ifndef BPXNU_LINUX_LIST_H
#define BPXNU_LINUX_LIST_H
#include <stddef.h>
#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
struct list_head {
	struct list_head *next, *prev;
};
#define LIST_HEAD(name) struct list_head name = { &(name), &(name) }
static inline void list_add(struct list_head *entry, struct list_head *head)
{
	entry->next = head->next;
	entry->prev = head;
	head->next->prev = entry;
	head->next = entry;
}
static inline void list_del(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	entry->next = entry->prev = NULL;
}
#define list_for_each_entry(pos, head, member)                                   \
	for (pos = container_of((head)->next, __typeof__(*pos), member);         \
	     &pos->member != (head);                                              \
	     pos = container_of(pos->member.next, __typeof__(*pos), member))
#endif
