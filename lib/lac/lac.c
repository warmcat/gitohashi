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

#include "../private.h"

void
list_ptr_insert(list_ptr *head, list_ptr *add, list_ptr_sort_func_t sort_func)
{
	while (sort_func && *head) {
		if (sort_func(add, *head) <= 0)
			break;

		head = *head;
	}

	*add = *head;
	*head = add;
}

size_t
lac_align(size_t length)
{
	size_t align = sizeof(int *);

	if (length & (align - 1))
		length += align - (length & (align - 1));

	return length;
}

void *
lac_use(struct lac **head, size_t ensure, size_t chunk_size)
{
	struct lac *chunk;
	size_t ofs, alloc;

	if (!chunk_size)
		alloc = LAC_CHUNK_SIZE + sizeof(*chunk);
	else
		alloc = chunk_size + sizeof(*chunk);

	/* if we meet something outside our expectation, allocate to meet it */

	if (ensure >= alloc - sizeof(*chunk))
		alloc = ensure + sizeof(*chunk);

	/* ensure there's a chunk and enough space in it for this name */

	if (!*head || (*head)->curr->alloc_size - (*head)->curr->ofs < ensure) {
		chunk = malloc(alloc);
		if (!chunk) {
			lwsl_err("OOM\n");
			return NULL;
		}

		if (!*head)
			*head = chunk;
		else
			(*head)->curr->next = chunk;

		(*head)->curr = chunk;
		(*head)->curr->head = *head;

		chunk->next = NULL;
		chunk->alloc_size = alloc;
		chunk->detached = 0;
		chunk->refcount = 0;

		/*
		 * belabouring the point... ofs is aligned to the platform's
		 * generic struct alignment at the start then
		 */
		(*head)->curr->ofs = sizeof(*chunk);
	}

	ofs = (*head)->curr->ofs;

	(*head)->curr->ofs += lac_align(ensure);
	if ((*head)->curr->ofs >= (*head)->curr->alloc_size)
		(*head)->curr->ofs = (*head)->curr->alloc_size;

	return (char *)(*head)->curr + ofs;
}

void
lac_free(struct lac **head)
{
	struct lac *it = *head;

	while (it) {
		struct lac *tmp = it->next;

		free(it);
		it = tmp;
	}

	*head = NULL;
}

void
lac_use_start(struct lac **iter,
		  struct lac *head)
{
	head->refcount++;
	*iter = head;
}

void
lac_use_end(struct lac *any)
{
	any->head->refcount--;
	if (any->head->detached && !any->head->refcount)
		lac_free(&any->head);
}

void
lac_detach(struct lac *any)
{
	any->head->detached = 1;
	if (!any->head->refcount)
		lac_free(&any->head);
}
