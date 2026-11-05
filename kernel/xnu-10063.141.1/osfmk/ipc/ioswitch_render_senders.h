/*
 * IOSwitch: the mach messages the render server (backboardd) receives, by sending process and message id, for the
 * display's per-window clients line (NativeLegacyDisplay). Core Animation's clients send each transaction they commit to
 * the render server as a mach message, so the rate of a client's messages is the rate it draws at, whether by design (a
 * widget at 20 Hz, a display link asking 10 Hz) or because it is late. Messages of other kinds show up under their own ids.
 *
 * Plain data, no locking: ipc_kmsg.c holds its spin lock around note and take; kernel/tests runs it on the host.
 */
#ifndef _IPC_IOSWITCH_RENDER_SENDERS_H_
#define _IPC_IOSWITCH_RENDER_SENDERS_H_

#include <stddef.h>
#include <stdint.h>

#define IOSWITCH_RENDER_SENDERS 32
#define IOSWITCH_RENDER_NAME    17

struct ioswitch_render_sender {
	int32_t pid;
	int32_t id;
	uint32_t count;
	char name[IOSWITCH_RENDER_NAME];
};

struct ioswitch_render_senders {
	struct ioswitch_render_sender entry[IOSWITCH_RENDER_SENDERS];
	uint32_t used;
	uint32_t dropped; /* messages of (pid, id) pairs past a full table */
};

static inline void
ioswitch_render_senders_note(struct ioswitch_render_senders *t, int32_t pid, int32_t id, const char *name)
{
	uint32_t i;
	for (i = 0; i < t->used; i++) {
		if (t->entry[i].pid == pid && t->entry[i].id == id) {
			t->entry[i].count++;
			return;
		}
	}
	if (t->used == IOSWITCH_RENDER_SENDERS) {
		t->dropped++;
		return;
	}
	struct ioswitch_render_sender *e = &t->entry[t->used++];
	e->pid = pid;
	e->id = id;
	e->count = 1;
	for (i = 0; i + 1 < IOSWITCH_RENDER_NAME && name && name[i]; i++) {
		e->name[i] = name[i];
	}
	e->name[i] = 0;
}

/* Per second over window_ms, in tenths */
static inline uint64_t
ioswitch_render_tenths(uint64_t count, uint64_t window_ms)
{
	return count * 10000 / (window_ms ? window_ms : 1);
}

/*
 * The `top` processes by messages (a process's ids summed), largest first, each with its rate and its two most frequent
 * ids: "SpringBoard[31] 10.0/s (#40002 9.8, #40017 0.2)", then the rest as "others N.N/s". Writes a NUL-terminated line of
 * at most size bytes and returns its length (0: nothing was sent).
 */
static inline size_t
ioswitch_render_senders_format(const struct ioswitch_render_senders *t, uint64_t window_ms, unsigned top, char *out,
    size_t size)
{
	uint32_t total[IOSWITCH_RENDER_SENDERS] = {0}, i, j;
	uint8_t first[IOSWITCH_RENDER_SENDERS] = {0}; /* an entry is its pid's first */
	uint64_t others = t->dropped;
	size_t at = 0;

	if (!size) {
		return 0;
	}
	out[0] = 0;
	for (i = 0; i < t->used; i++) {
		for (j = 0; j < i && t->entry[j].pid != t->entry[i].pid; j++) {
		}
		first[i] = j == i;
		total[j] += t->entry[i].count;
	}
	for (unsigned shown = 0;; shown++) {
		uint32_t best = IOSWITCH_RENDER_SENDERS;
		for (i = 0; i < t->used; i++) {
			if (first[i] && total[i] && (best == IOSWITCH_RENDER_SENDERS || total[i] > total[best])) {
				best = i;
			}
		}
		if (best == IOSWITCH_RENDER_SENDERS) {
			break;
		}
		if (shown >= top) {
			others += total[best];
			total[best] = 0;
			continue;
		}
		/* its two most frequent ids */
		uint32_t a = IOSWITCH_RENDER_SENDERS, b = IOSWITCH_RENDER_SENDERS;
		for (i = 0; i < t->used; i++) {
			if (t->entry[i].pid != t->entry[best].pid) {
				continue;
			}
			if (a == IOSWITCH_RENDER_SENDERS || t->entry[i].count > t->entry[a].count) {
				b = a;
				a = i;
			} else if (b == IOSWITCH_RENDER_SENDERS || t->entry[i].count > t->entry[b].count) {
				b = i;
			}
		}
		const uint64_t r = ioswitch_render_tenths(total[best], window_ms),
		    ra = ioswitch_render_tenths(t->entry[a].count, window_ms);
		int n = snprintf(out + at, size - at, "%s%s[%d] %llu.%llu/s (#%d %llu.%llu", at ? ", " : "", t->entry[best].name,
		    t->entry[best].pid, (unsigned long long)(r / 10), (unsigned long long)(r % 10), t->entry[a].id,
		    (unsigned long long)(ra / 10), (unsigned long long)(ra % 10));
		if (n > 0) {
			at += (size_t)n < size - at ? (size_t)n : size - at - 1;
		}
		if (b != IOSWITCH_RENDER_SENDERS && at < size - 1) {
			const uint64_t rb = ioswitch_render_tenths(t->entry[b].count, window_ms);
			n = snprintf(out + at, size - at, ", #%d %llu.%llu", t->entry[b].id, (unsigned long long)(rb / 10),
			    (unsigned long long)(rb % 10));
			if (n > 0) {
				at += (size_t)n < size - at ? (size_t)n : size - at - 1;
			}
		}
		if (at < size - 1) {
			n = snprintf(out + at, size - at, ")");
			if (n > 0) {
				at += (size_t)n < size - at ? (size_t)n : size - at - 1;
			}
		}
		total[best] = 0;
	}
	if (others && at < size - 1) {
		const uint64_t r = ioswitch_render_tenths(others, window_ms);
		int n = snprintf(out + at, size - at, "%sothers %llu.%llu/s", at ? ", " : "", (unsigned long long)(r / 10),
		    (unsigned long long)(r % 10));
		if (n > 0) {
			at += (size_t)n < size - at ? (size_t)n : size - at - 1;
		}
	}
	return at;
}

#ifdef XNU_KERNEL_PRIVATE
struct task;
/* ipc_kmsg.c: the render server's task (NULL: none), and the window's senders as one line, emptied for the next window */
void ioswitch_render_server_watch(struct task *task);
size_t ioswitch_render_senders_line(char *out, size_t size, uint64_t window_ms, unsigned top);
#endif

#endif /* _IPC_IOSWITCH_RENDER_SENDERS_H_ */
