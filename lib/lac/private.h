/*
 * libjsongit2 - linear alloc chunk
 *
 * Copyright (C) 2018 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include <sys/stat.h>

/* under page size of 4096 to allow overhead */
#define LAC_CHUNK_SIZE 4000

#define list_ptr_container(P,T,M) ((T *)((char *)(P) - offsetof(T, M)))

/*
 * These lists point to their corresponding "next" member in the target, NOT
 * the original containing struct.  To get the containing struct, you must use
 * list_ptr_container() to convert.
 *
 * It's like that because it means we no longer have to have the next pointer
 * at the start of the struct, and we can have the same struct on multiple
 * linked-lists with everything held in the struct itself.
 */
typedef void * list_ptr;

/*
 * optional sorting callback called by list_ptr_insert() to sort the right
 * things inside the opqaue struct being sorted / inserted on the list.
 */
typedef int (*list_ptr_sort_func_t)(list_ptr a, list_ptr b);

#define list_ptr_advance(_lp) _lp = *((void **)_lp)

/* sort may be NULL if you don't care about order */
void
list_ptr_insert(list_ptr *phead, list_ptr *add, list_ptr_sort_func_t sort);


/*
 * the chunk list members all point back to the head themselves so the list
 * can be detached from the formal head and free itself when its reference
 * count reaches zero.
 */

struct lac {
	struct lac *next;
	struct lac *head; /* pointer back to the first chunk */
	struct lac *curr; /* applies to head chunk only */
	size_t alloc_size;
	size_t ofs; /* next writeable position inside chunk */
	int refcount; /* applies to head chunk only */
	char detached; /* if our refcount gets to zero, free the chunk list */
};

typedef unsigned char * lac_cached_file_t;

struct cached_file_info {
	struct stat s;
	time_t last_confirm;
};

/* chunk_size of 0 allocates ensure + overhead */

void *
lac_use(struct lac **head, size_t ensure, size_t chunk_size);

size_t
lac_align(size_t length);

void
lac_free(struct lac **head);

void
lac_use_start(struct lac **iter, struct lac *head);

void
lac_use_end(struct lac *any);

void
lac_detach(struct lac *any);

void
lac_use_cached_file_start(lac_cached_file_t cache);

void
lac_use_cached_file_end(lac_cached_file_t *cache);

void
lac_use_cached_file_detach(lac_cached_file_t *cache);

int
lac_cached_file(const char *filepath, lac_cached_file_t *cache, size_t *len);
