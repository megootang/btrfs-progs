/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */
#define _XOPEN_SOURCE 600
#define __USE_XOPEN2K
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "kerncompat.h"
#include "extent_map.h"
#include "list.h"

u64 cache_max = 1024 * 1024 * 32;

void extent_map_tree_init(struct extent_map_tree *tree)
{
	cache_tree_init(&tree->state);
	cache_tree_init(&tree->cache);
	INIT_LIST_HEAD(&tree->lru);
	tree->cache_size = 0;
}

static struct extent_state *alloc_extent_state(void)
{
	struct extent_state *state;

	state = malloc(sizeof(*state));
	if (!state)
		return NULL;
	state->refs = 1;
	state->state = 0;
	state->private = 0;
	return state;
}

static void free_extent_state(struct extent_state *state)
{
	state->refs--;
	BUG_ON(state->refs < 0);
	if (state->refs == 0)
		free(state);
}

void extent_map_tree_cleanup(struct extent_map_tree *tree)
{
	struct extent_state *es;
	struct extent_buffer *eb;
	struct cache_extent *cache;

	while(!list_empty(&tree->lru)) {
		eb = list_entry(tree->lru.next, struct extent_buffer, lru);
		if (eb->refs != 1) {
			fprintf(stderr, "extent buffer leak: "
				"start %Lu len %u\n", eb->start, eb->len);
			eb->refs = 1;
		}
		free_extent_buffer(eb);
	}
	while (1) {
		cache = find_first_cache_extent(&tree->state, 0);
		if (!cache)
			break;
		es = container_of(cache, struct extent_state, cache_node);
		remove_cache_extent(&tree->state, &es->cache_node);
		free_extent_state(es);
	}
}

static inline void update_extent_state(struct extent_state *state)
{
	state->cache_node.start = state->start;
	state->cache_node.size = state->end + 1 - state->start;
}

/*
 * Utility function to look for merge candidates inside a given range.
 * Any extents with matching state are merged together into a single
 * extent in the tree. Extents with EXTENT_IO in their state field are
 * not merged
 */
static int merge_state(struct extent_map_tree *tree,
		       struct extent_state *state)
{
	struct extent_state *other;
	struct cache_extent *other_node;

	if (state->state & EXTENT_IOBITS)
		return 0;

	other_node = prev_cache_extent(&state->cache_node);
	if (other_node) {
		other = container_of(other_node, struct extent_state,
				     cache_node);
		if (other->end == state->start - 1 &&
		    other->state == state->state) {
			state->start = other->start;
			update_extent_state(state);
			remove_cache_extent(&tree->state, &other->cache_node);
			free_extent_state(other);
		}
	}
	other_node = next_cache_extent(&state->cache_node);
	if (other_node) {
		other = container_of(other_node, struct extent_state,
				     cache_node);
		if (other->start == state->end + 1 &&
		    other->state == state->state) {
			other->start = state->start;
			update_extent_state(other);
			remove_cache_extent(&tree->state, &state->cache_node);
			free_extent_state(state);
		}
	}
	return 0;
}

/*
 * insert an extent_state struct into the tree.  'bits' are set on the
 * struct before it is inserted.
 */
static int insert_state(struct extent_map_tree *tree,
			struct extent_state *state, u64 start, u64 end,
			int bits)
{
	int ret;

	BUG_ON(end < start);
	state->state |= bits;
	state->start = start;
	state->end = end;
	update_extent_state(state);
	ret = insert_existing_cache_extent(&tree->state, &state->cache_node);
	BUG_ON(ret);
	merge_state(tree, state);
	return 0;
}

/*
 * split a given extent state struct in two, inserting the preallocated
 * struct 'prealloc' as the newly created second half.  'split' indicates an
 * offset inside 'orig' where it should be split.
 */
static int split_state(struct extent_map_tree *tree, struct extent_state *orig,
		       struct extent_state *prealloc, u64 split)
{
	int ret;
	prealloc->start = orig->start;
	prealloc->end = split - 1;
	prealloc->state = orig->state;
	update_extent_state(prealloc);
	orig->start = split;
	update_extent_state(orig);
	ret = insert_existing_cache_extent(&tree->state,
					   &prealloc->cache_node);
	BUG_ON(ret);
	return 0;
}

/*
 * clear some bits on a range in the tree.
 */
static int clear_state_bit(struct extent_map_tree *tree,
			    struct extent_state *state, int bits)
{
	int ret = state->state & bits;

	state->state &= ~bits;
	if (state->state == 0) {
		remove_cache_extent(&tree->state, &state->cache_node);
		free_extent_state(state);
	} else {
		merge_state(tree, state);
	}
	return ret;
}

/*
 * set some bits on a range in the tree.
 */
int clear_extent_bits(struct extent_map_tree *tree, u64 start,
		      u64 end, int bits, gfp_t mask)
{
	struct extent_state *state;
	struct extent_state *prealloc = NULL;
	struct cache_extent *node;
	int err;
	int set = 0;

again:
	prealloc = alloc_extent_state();
	if (!prealloc)
		return -ENOMEM;

	/*
	 * this search will find the extents that end after
	 * our range starts
	 */
	node = find_first_cache_extent(&tree->state, start);
	if (!node)
		goto out;
	state = container_of(node, struct extent_state, cache_node);
	if (state->start > end)
		goto out;

	/*
	 *     | ---- desired range ---- |
	 *  | state | or
	 *  | ------------- state -------------- |
	 *
	 * We need to split the extent we found, and may flip
	 * bits on second half.
	 *
	 * If the extent we found extends past our range, we
	 * just split and search again.  It'll get split again
	 * the next time though.
	 *
	 * If the extent we found is inside our range, we clear
	 * the desired bit on it.
	 */
	if (state->start < start) {
		err = split_state(tree, state, prealloc, start);
		BUG_ON(err == -EEXIST);
		prealloc = NULL;
		if (err)
			goto out;
		if (state->end <= end) {
			start = state->end + 1;
			set |= clear_state_bit(tree, state, bits);
		} else {
			start = state->start;
		}
		goto search_again;
	}
	/*
	 * | ---- desired range ---- |
	 *                        | state |
	 * We need to split the extent, and clear the bit
	 * on the first half
	 */
	if (state->start <= end && state->end > end) {
		err = split_state(tree, state, prealloc, end + 1);
		BUG_ON(err == -EEXIST);

		set |= clear_state_bit(tree, prealloc, bits);
		prealloc = NULL;
		goto out;
	}

	start = state->end + 1;
	set |= clear_state_bit(tree, state, bits);
	goto search_again;
out:
	if (prealloc)
		free_extent_state(prealloc);
	return set;

search_again:
	if (start > end)
		goto out;
	goto again;
}

/*
 * set some bits on a range in the tree.
 */
int set_extent_bits(struct extent_map_tree *tree, u64 start,
		    u64 end, int bits, gfp_t mask)
{
	struct extent_state *state;
	struct extent_state *prealloc = NULL;
	struct cache_extent *node;
	int err = 0;
	int set;
	u64 last_start;
	u64 last_end;
again:
	prealloc = alloc_extent_state();
	if (!prealloc)
		return -ENOMEM;

	/*
	 * this search will find the extents that end after
	 * our range starts
	 */
	node = find_first_cache_extent(&tree->state, start);
	if (!node) {
		err = insert_state(tree, prealloc, start, end, bits);
		BUG_ON(err == -EEXIST);
		prealloc = NULL;
		goto out;
	}

	state = container_of(node, struct extent_state, cache_node);
	last_start = state->start;
	last_end = state->end;

	/*
	 * | ---- desired range ---- |
	 * | state |
	 *
	 * Just lock what we found and keep going
	 */
	if (state->start == start && state->end <= end) {
		set = state->state & bits;
		state->state |= bits;
		start = state->end + 1;
		merge_state(tree, state);
		goto search_again;
	}
	/*
	 *     | ---- desired range ---- |
	 * | state |
	 *   or
	 * | ------------- state -------------- |
	 *
	 * We need to split the extent we found, and may flip bits on
	 * second half.
	 *
	 * If the extent we found extends past our
	 * range, we just split and search again.  It'll get split
	 * again the next time though.
	 *
	 * If the extent we found is inside our range, we set the
	 * desired bit on it.
	 */
	if (state->start < start) {
		set = state->state & bits;
		err = split_state(tree, state, prealloc, start);
		BUG_ON(err == -EEXIST);
		prealloc = NULL;
		if (err)
			goto out;
		if (state->end <= end) {
			state->state |= bits;
			start = state->end + 1;
			merge_state(tree, state);
		} else {
			start = state->start;
		}
		goto search_again;
	}
	/*
	 * | ---- desired range ---- |
	 *     | state | or               | state |
	 *
	 * There's a hole, we need to insert something in it and
	 * ignore the extent we found.
	 */
	if (state->start > start) {
		u64 this_end;
		if (end < last_start)
			this_end = end;
		else
			this_end = last_start -1;
		err = insert_state(tree, prealloc, start, this_end,
				bits);
		BUG_ON(err == -EEXIST);
		prealloc = NULL;
		if (err)
			goto out;
		start = this_end + 1;
		goto search_again;
	}
	/*
	 * | ---- desired range ---- |
	 * | ---------- state ---------- |
	 * We need to split the extent, and set the bit
	 * on the first half
	 */
	set = state->state & bits;
	err = split_state(tree, state, prealloc, end + 1);
	BUG_ON(err == -EEXIST);

	state->state |= bits;
	merge_state(tree, prealloc);
	prealloc = NULL;
out:
	if (prealloc)
		free_extent_state(prealloc);
	return err;
search_again:
	if (start > end)
		goto out;
	goto again;
}

int set_extent_dirty(struct extent_map_tree *tree, u64 start, u64 end,
		     gfp_t mask)
{
	return set_extent_bits(tree, start, end, EXTENT_DIRTY, mask);
}

int clear_extent_dirty(struct extent_map_tree *tree, u64 start, u64 end,
		       gfp_t mask)
{
	return clear_extent_bits(tree, start, end, EXTENT_DIRTY, mask);
}

int find_first_extent_bit(struct extent_map_tree *tree, u64 start,
			  u64 *start_ret, u64 *end_ret, int bits)
{
	struct cache_extent *node;
	struct extent_state *state;
	int ret = 1;

	/*
	 * this search will find all the extents that end after
	 * our range starts.
	 */
	node = find_first_cache_extent(&tree->state, start);
	if (!node)
		goto out;

	while(1) {
		state = container_of(node, struct extent_state, cache_node);
		if (state->end >= start && (state->state & bits)) {
			*start_ret = state->start;
			*end_ret = state->end;
			ret = 0;
			break;
		}
		node = next_cache_extent(node);
		if (!node)
			break;
	}
out:
	return ret;
}

int test_range_bit(struct extent_map_tree *tree, u64 start, u64 end,
		   int bits, int filled)
{
	struct extent_state *state = NULL;
	struct cache_extent *node;
	int bitset = 0;

	node = find_first_cache_extent(&tree->state, start);
	while (node && start <= end) {
		state = container_of(node, struct extent_state, cache_node);

		if (filled && state->start > start) {
			bitset = 0;
			break;
		}
		if (state->start > end)
			break;
		if (state->state & bits) {
			bitset = 1;
			if (!filled)
				break;
		} else if (filled) {
			bitset = 0;
			break;
		}
		start = state->end + 1;
		if (start > end)
			break;
		node = next_cache_extent(node);
	}
	return bitset;
}

int set_state_private(struct extent_map_tree *tree, u64 start, u64 private)
{
	struct cache_extent *node;
	struct extent_state *state;
	int ret = 0;

	node = find_first_cache_extent(&tree->state, start);
	if (!node) {
		ret = -ENOENT;
		goto out;
	}
	state = container_of(node, struct extent_state, cache_node);
	if (state->start != start) {
		ret = -ENOENT;
		goto out;
	}
	state->private = private;
out:
	return ret;
}

int get_state_private(struct extent_map_tree *tree, u64 start, u64 *private)
{
	struct cache_extent *node;
	struct extent_state *state;
	int ret = 0;

	node = find_first_cache_extent(&tree->state, start);
	if (!node) {
		ret = -ENOENT;
		goto out;
	}
	state = container_of(node, struct extent_state, cache_node);
	if (state->start != start) {
		ret = -ENOENT;
		goto out;
	}
	*private = state->private;
out:
	return ret;
}

static int free_some_buffers(struct extent_map_tree *tree)
{
	u32 nrscan = 0;
	struct extent_buffer *eb;
	struct list_head *node, *next;

	if (tree->cache_size < cache_max)
		return 0;
	list_for_each_safe(node, next, &tree->lru) {
		eb = list_entry(node, struct extent_buffer, lru);
		if (eb->refs == 1) {
			free_extent_buffer(eb);
			if (tree->cache_size < cache_max)
				break;
		}
		if (nrscan++ > 64)
			break;
	}
	return 0;
}

static struct extent_buffer *__alloc_extent_buffer(struct extent_map_tree *tree,
						   u64 bytenr, u32 blocksize)
{
	struct extent_buffer *eb;
	int ret;

	eb = malloc(sizeof(struct extent_buffer) + blocksize);
	if (!eb)
		return NULL;

	eb->start = bytenr;
	eb->len = blocksize;
	eb->refs = 2;
	eb->flags = 0;
	eb->tree = tree;
	eb->fd = -1;
	eb->dev_bytenr = (u64)-1;
	eb->cache_node.start = bytenr;
	eb->cache_node.size = blocksize;

	free_some_buffers(tree);
	ret = insert_existing_cache_extent(&tree->cache, &eb->cache_node);
	if (ret) {
		free(eb);
		return NULL;
	}
	list_add_tail(&eb->lru, &tree->lru);
	tree->cache_size += blocksize;
	return eb;
}

void free_extent_buffer(struct extent_buffer *eb)
{
	if (!eb)
		return;

	eb->refs--;
	BUG_ON(eb->refs < 0);
	if (eb->refs == 0) {
		struct extent_map_tree *tree = eb->tree;
		BUG_ON(eb->flags & EXTENT_DIRTY);
		list_del_init(&eb->lru);
		remove_cache_extent(&tree->cache, &eb->cache_node);
		BUG_ON(tree->cache_size < eb->len);
		tree->cache_size -= eb->len;
		free(eb);
	}
}

struct extent_buffer *find_extent_buffer(struct extent_map_tree *tree,
					 u64 bytenr, u32 blocksize)
{
	struct extent_buffer *eb = NULL;
	struct cache_extent *cache;

	cache = find_cache_extent(&tree->cache, bytenr, blocksize);
	if (cache && cache->start == bytenr && cache->size == blocksize) {
		eb = container_of(cache, struct extent_buffer, cache_node);
		list_move_tail(&eb->lru, &tree->lru);
		eb->refs++;
	}
	return eb;
}

struct extent_buffer *find_first_extent_buffer(struct extent_map_tree *tree,
					       u64 start)
{
	struct extent_buffer *eb = NULL;
	struct cache_extent *cache;

	cache = find_first_cache_extent(&tree->cache, start);
	if (cache) {
		eb = container_of(cache, struct extent_buffer, cache_node);
		list_move_tail(&eb->lru, &tree->lru);
		eb->refs++;
	}
	return eb;
}

struct extent_buffer *alloc_extent_buffer(struct extent_map_tree *tree,
					  u64 bytenr, u32 blocksize)
{
	struct extent_buffer *eb;
	struct cache_extent *cache;

	cache = find_cache_extent(&tree->cache, bytenr, blocksize);
	if (cache && cache->start == bytenr && cache->size == blocksize) {
		eb = container_of(cache, struct extent_buffer, cache_node);
		list_move_tail(&eb->lru, &tree->lru);
		eb->refs++;
	} else {
		if (cache) {
			eb = container_of(cache, struct extent_buffer,
					  cache_node);
			BUG_ON(eb->refs != 1);
			free_extent_buffer(eb);
		}
		eb = __alloc_extent_buffer(tree, bytenr, blocksize);
	}
	return eb;
}

int read_extent_from_disk(struct extent_buffer *eb)
{
	int ret;
	ret = pread(eb->fd, eb->data, eb->len, eb->dev_bytenr);
	if (ret < 0)
		goto out;
	if (ret != eb->len) {
		ret = -EIO;
		goto out;
	}
	ret = 0;
out:
	return ret;
}

int write_extent_to_disk(struct extent_buffer *eb)
{
	int ret;
	ret = pwrite(eb->fd, eb->data, eb->len, eb->dev_bytenr);
	if (ret < 0)
		goto out;
	if (ret != eb->len) {
		ret = -EIO;
		goto out;
	}
	ret = 0;
out:
	return ret;
}

int set_extent_buffer_uptodate(struct extent_buffer *eb)
{
	eb->flags |= EXTENT_UPTODATE;
	return 0;
}

int extent_buffer_uptodate(struct extent_buffer *eb)
{
	if (eb->flags & EXTENT_UPTODATE)
		return 1;
	return 0;
}

int set_extent_buffer_dirty(struct extent_buffer *eb)
{
	struct extent_map_tree *tree = eb->tree;
	if (!(eb->flags & EXTENT_DIRTY)) {
		eb->flags |= EXTENT_DIRTY;
		set_extent_dirty(tree, eb->start, eb->start + eb->len - 1, 0);
		extent_buffer_get(eb);
	}
	return 0;
}

int clear_extent_buffer_dirty(struct extent_buffer *eb)
{
	struct extent_map_tree *tree = eb->tree;
	if (eb->flags & EXTENT_DIRTY) {
		eb->flags &= ~EXTENT_DIRTY;
		clear_extent_dirty(tree, eb->start, eb->start + eb->len - 1, 0);
		free_extent_buffer(eb);
	}
	return 0;
}

int memcmp_extent_buffer(struct extent_buffer *eb, const void *ptrv,
			 unsigned long start, unsigned long len)
{
	return memcmp(eb->data + start, ptrv, len);
}

void read_extent_buffer(struct extent_buffer *eb, void *dst,
			unsigned long start, unsigned long len)
{
	memcpy(dst, eb->data + start, len);
}

void write_extent_buffer(struct extent_buffer *eb, const void *src,
			 unsigned long start, unsigned long len)
{
	memcpy(eb->data + start, src, len);
}

void copy_extent_buffer(struct extent_buffer *dst, struct extent_buffer *src,
			unsigned long dst_offset, unsigned long src_offset,
			unsigned long len)
{
	memcpy(dst->data + dst_offset, src->data + src_offset, len);
}

void memcpy_extent_buffer(struct extent_buffer *dst, unsigned long dst_offset,
			  unsigned long src_offset, unsigned long len)
{
	memcpy(dst->data + dst_offset, dst->data + src_offset, len);
}

void memmove_extent_buffer(struct extent_buffer *dst, unsigned long dst_offset,
			   unsigned long src_offset, unsigned long len)
{
	memmove(dst->data + dst_offset, dst->data + src_offset, len);
}

void memset_extent_buffer(struct extent_buffer *eb, char c,
			  unsigned long start, unsigned long len)
{
	memset(eb->data + start, c, len);
}