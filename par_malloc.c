#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <pthread.h>

#include "xmalloc.h"

typedef struct header {
	size_t size; // size of the entire allocated portion (including size field)
	struct header* next;
} header;

const size_t PAGE_SIZE = 4096;
// I looked up how to do powers of two using bit operations and I use it throughout the code 
// found here: https://www.quora.com/How-do-I-make-a-program-in-C-that-prints-2-raise-to-the-power-n
const size_t GLOBAL_SIZE_MAX = (1 << 30); // 2^30
const int LOCAL_BUCKETS = 20;
const size_t LOCAL_SIZE_MAX = (1 << 20); // 2^20

header* global_list = 0; // sorted by memory address
pthread_mutex_t mutex; // used to lock global_list
int init = 0;

// each bucket sorted by memory address
__thread header* buckets[21] = {0}; // biggest bucket is 2^20 - initialized to nulls


// for troubleshooting
void
check_buckets(const char* message) {
	for (int i = 2; i < 21; ++i) {
		header* cur = buckets[i];
		while(cur) {
			if(cur->size != (1 << i)) {
				printf("%s\n", message);
				printf("size %ld found in bucket %d\n", cur->size, i);
				assert(0);
			}
			cur = cur->next;
		}
	}
}

// Takes the log_2(bytes) rounded up (i.e. returns x s.t. 2^x >= bytes)
// minimum return value is log_2(sizeof(header)) since sizeof(header) is the min allocation size
int
log_up(size_t size) {
	if (size < sizeof(header)) size = sizeof(header);
	int out = 3;
	for(; (1 << out) < size; ++out);
	return out;
}

// division rounded up
size_t
div_up(size_t x, size_t y) {
	assert(y != 0);
	size_t z = x / y;
	if (z * y == x) return z;
	else return z + 1;
}

// splits the given block into two parts, one of size size, and the other is the rest
// returns a pointer to the second portion of the block
// returns 0 if there's not enough space left for a second portion
header*
split_block(header* block, size_t size) {
	if (block->size == size) {
		return 0;
	}
	else {
		header* block2 = (header*)((long)block + size);
		block2->size = block->size - size;
		block->size = size;
		return block2;
	}
}

// finds a block in the global list that is the given size or larger and removes it from the list
// maps a new one if none such block exists
// Note: this uses the lock
header*
find_global_block(size_t size) {
	// rounds up size to the nearest multiple of LOCAL_SIZE_MAX
	size_t num_blocks = div_up(size, LOCAL_SIZE_MAX);
	size = num_blocks * LOCAL_SIZE_MAX;
	
	int rv = pthread_mutex_lock(&mutex);
	assert(rv == 0);
	header* out = 0;

	if (global_list->size >= size) {
		out = global_list;
		header* free_block = split_block(out, size);
		if (free_block != 0) {
			free_block->next = out->next;
			global_list = free_block;		
		} else {
			global_list = out->next;
		}
	} else {
		header* cur = global_list;
		while((long)(cur->next) != 0) {
			if (cur->next->size >= size) {
				out = cur->next;
				header* free_block = split_block(out, size);
				if (free_block != 0) {
					cur->next = free_block;
				} else {
					cur->next = out->next;
					break;
				}
			}
			cur = cur->next;
		}
	}

	rv = pthread_mutex_unlock(&mutex);
	assert(rv == 0);
	if (out == 0) {
		out = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert((long)out != -1);
		out->size = (size);
	}
	return out;
}

// finds a block in the local array and removes it
// returns block with correct size, etc.
header*
find_local_block(size_t size) {
	// rounds up size to the nearest power of 2
	int log = log_up(size);
	size = (1 << log);
	assert(log <= LOCAL_BUCKETS);
	
	header* out = 0;
	// check in correct bucket
	if (buckets[log] != 0) {
		out = buckets[log];
		buckets[log] = out->next;
	} else {
		// find the next lowest bucket with free memory
		int i = log;
		for (; i <= LOCAL_BUCKETS; ++i) {
			if (buckets[i] != 0) break;
		}
		header* tmp;
		if (i > LOCAL_BUCKETS) { // need to get more mem into cache
			i = LOCAL_BUCKETS;
			buckets[i] = find_global_block(LOCAL_SIZE_MAX);
			(buckets[i])->next = 0;
		}
		// split up memory into lower buckets
		// we know none of these buckets have any memory
		for (; i > log; --i) {
			// split up first element and move it down
			header** tmp = buckets;
			buckets[i-1] = buckets[i];
			buckets[i] = (buckets[i])->next;
			(buckets[i-1])->next = split_block(buckets[i-1], (1 << (i-1)));
			(buckets[i-1])->next->next = 0;
		}
		assert(buckets[log] != 0);
		out = buckets[log];
		buckets[log] = out->next;
	}
	return out;
}
	
void*
xmalloc(size_t bytes)
{
	// initialize if first call
	if (init == 0) {
		global_list = mmap(0, GLOBAL_SIZE_MAX, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert((long)global_list != 0);
		global_list->size = GLOBAL_SIZE_MAX;
		global_list->next = 0;

		pthread_mutex_init(&mutex, 0);
		init = 1;
	}

	header* block;
	size_t size = bytes + sizeof(size_t);
	if (size > LOCAL_SIZE_MAX) {
		block = find_global_block(size);
	} else {
		block = find_local_block(size);
	}
	return (void*)((long)block + sizeof(size_t));
}

// tries to coalesce the given block with its own next block
// returns 0 if no coalesce occurred, else 1
int
coalesce_next(header* block) {
	if ((long)block + block->size == (long)(block->next)) {
		header* next = block->next;
		assert((long)next != 0);
		block->size = block->size + next->size;
		block->next = next->next;
		return 1;
	} else {
		return 0;
	}
}

// inserts the given block into the global list by memory address
// coalesces
// Note: locks the global list
void
insert_global_block(header* block) {
	int rv = pthread_mutex_lock(&mutex);
	assert(rv == 0);

	if ((long) block < (long)global_list || (long)global_list == 0) {
		block->next = global_list;
		coalesce_next(block);
		global_list = block;
	} else {
		header* cur = global_list;
		while (cur->next != 0) {
			if ((long)block < (long)(cur->next)) {
				block->next = cur->next;
				cur->next = block;
				coalesce_next(block);
				coalesce_next(cur);
				rv = pthread_mutex_unlock(&mutex);
				assert(rv == 0);
				return;
			}
			cur = cur->next;
		}
		cur->next = block;
		block->next = 0;
		coalesce_next(cur);
	}

	rv = pthread_mutex_unlock(&mutex);
	assert(rv == 0);
}

void 
insert_local_block(header* block) {
	int log = log_up(block->size);
	int coalesce_after = (long)block % (1 << (log + 1)) == 0;
	// inserting into top bucket or above
	if (log == LOCAL_BUCKETS && buckets[LOCAL_BUCKETS] != 0) {
		insert_global_block(block);
	// if block should go at the front of list
	} else if ((long)block < (long)(buckets[log]) || (long)(buckets[log]) == 0) {
		block->next = buckets[log];
		if (coalesce_after && coalesce_next(block)) {
			buckets[log] = block->next;
			insert_local_block(block);
		} else {
			buckets[log] = block;
		}
	// else goes in the middle of the list
	} else {
		header* cur = buckets[log];
		header* prev = 0;
		while(cur->next != 0)  {
			if ((long)block < (long)(cur->next)) {
				block->next = cur->next;
				cur->next = block;
				if (coalesce_after) {
					int c = coalesce_next(block);
					if (c) {
						cur->next = block->next;
						insert_local_block(block);
					}
				} else {
					int c = coalesce_next(cur);
					if (c) {
						if (prev == 0) {
							buckets[log] = cur->next;
						} else {
							prev->next = cur->next;
						}
						insert_local_block(cur);
					}
				}
				return;
			}
			prev = cur;
			cur = cur->next;
		}	
		// goes at the end of the list
		cur->next = block;
		block->next = 0;
		if (!coalesce_after) {
			int c = coalesce_next(cur);
			if (c) {
				if (prev == 0) {
					buckets[log] = cur->next;
				} else {
					prev->next = cur->next;
				}
				insert_local_block(cur);
			}
		}
	}
}

void
xfree(void* ptr)
{
	assert(init == 1);
	header* block = (header*)((long)ptr - sizeof(size_t));
	if (block->size > LOCAL_SIZE_MAX) {
		insert_global_block(block);
	} else {
		insert_local_block(block);
	}
}

void*
xrealloc(void* prev, size_t bytes)
{
	assert(init == 1);
	header* block = (header*)((long)prev - sizeof(size_t));
	if (bytes == 0) {
		xfree(prev);
	} else {
		size_t num_blocks;
		size_t log;
		size_t size = bytes + sizeof(size_t);
		if (size > LOCAL_SIZE_MAX) {
			num_blocks = div_up(size, LOCAL_SIZE_MAX);
			size = num_blocks * LOCAL_SIZE_MAX;
		} else {
			log = log_up(size);
			size = (1 << log);
		}
		
		if (size == block->size) {
			return prev;
		} else if (size > block->size) {
			xfree(prev);
			return xmalloc(bytes);
		} else {
			if (size > LOCAL_SIZE_MAX) {
				header* free_block = split_block(block, size);
				insert_global_block(free_block);
				return (void*)((long)block + sizeof(size_t));
			} else {
				return prev;
			}
		}
	}
	return 0;
}

