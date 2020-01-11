#ifndef HMALLOC_H
#define HMALLOC_H

// cs3650 Starter Code

typedef struct xm_stats {
	long pages_mapped;
	long pages_unmapped;
	long chunks_allocated;
	long chunks_freed;
	long free_length;
} xm_stats;

xm_stats* xgetstats();
void xprintstats();

void* xmalloc(size_t size);
void xfree(void* item);
void* xrealloc(void* prev, size_t bytes);

#endif
