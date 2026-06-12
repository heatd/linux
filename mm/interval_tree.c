// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/interval_tree.c - interval tree for mapping->i_mmap
 *
 * Copyright (C) 2012, Michel Lespinasse <walken@google.com>
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/rmap.h>
#include <linux/interval_tree_generic.h>

static inline unsigned long vma_start_pgoff(struct vm_area_struct *v)
{
	return v->vm_pgoff;
}

INTERVAL_TREE_DEFINE(struct vm_area_struct, shared.rb,
		     unsigned long, shared.rb_subtree_last,
		     vma_start_pgoff, vma_last_pgoff, static, __vma_interval_tree)

static bool vma_has_rmap_bucket(const struct vm_area_struct *vma)
{
	return vma->vm_file_rmap_bucket != VM_NO_FILE_RMAP_BUCKET;
}

void vma_interval_tree_insert(struct vm_area_struct *node,
			      struct file_rmap *tree)
{
	VM_WARN_ON_ONCE_VMA(vma_has_rmap_bucket(node), node);
	node->vm_file_rmap_bucket = smp_processor_id() % NR_RMAP_SHARDS;
	__vma_interval_tree_insert(node, &tree->trees[node->vm_file_rmap_bucket]);
	tree->nr_vmas++;
}

void vma_interval_tree_remove(struct vm_area_struct *node,
			      struct file_rmap *tree)
{
	VM_WARN_ON_ONCE_VMA(!vma_has_rmap_bucket(node), node);
	__vma_interval_tree_remove(node, &tree->trees[node->vm_file_rmap_bucket]);
	node->vm_file_rmap_bucket = VM_NO_FILE_RMAP_BUCKET;
	tree->nr_vmas--;
}

struct vm_area_struct *vma_interval_tree_subtree_search(struct vm_area_struct *node,
				unsigned long start, unsigned long last)
{
	return __vma_interval_tree_subtree_search(node, start, last);
}

struct vm_area_struct *vma_interval_tree_iter_first(struct file_rmap *tree,
				unsigned long start, unsigned long last)
{
	struct vm_area_struct *vma;
	unsigned int i;

	for (i = 0; i < NR_RMAP_SHARDS; i++) {
		vma = __vma_interval_tree_iter_first(&tree->trees[i], start, last);
		if (vma)
			return vma;
	}

	return NULL;
}

struct vm_area_struct *vma_interval_tree_iter_next(struct file_rmap *tree,
						   struct vm_area_struct *node,
						   unsigned long start,
						   unsigned long last)
{
	struct vm_area_struct *vma;
	unsigned int i;

	vma = __vma_interval_tree_iter_next(node, start, last);
	if (vma)
		return vma;
	/*
	 * While we didn't run out of trees, attempt to get the first of each
	 * subtree. This will serve as the "next" for the caller. Note that
	 * this does not maintain any sort of file offset in-order.
	 */
	for (i = node->vm_file_rmap_bucket; i < NR_RMAP_SHARDS; i++) {
		vma = __vma_interval_tree_iter_first(&tree->trees[i], start, last);
		if (vma)
			return vma;
	}

	return NULL;
}
/* Insert node immediately after prev in the interval tree */
void vma_interval_tree_insert_after(struct vm_area_struct *node,
				    struct vm_area_struct *prev,
				    struct file_rmap *tree)
{
	struct rb_node **link;
	struct vm_area_struct *parent;
	unsigned long last = vma_last_pgoff(node);
	struct rb_root_cached *root = &tree->trees[node->vm_file_rmap_bucket];

	VM_BUG_ON_VMA(vma_start_pgoff(node) != vma_start_pgoff(prev), node);
	VM_WARN_ON_ONCE_VMA(node->vm_file_rmap_bucket != prev->vm_file_rmap_bucket,
		       node);
	if (!prev->shared.rb.rb_right) {
		parent = prev;
		link = &prev->shared.rb.rb_right;
	} else {
		parent = rb_entry(prev->shared.rb.rb_right,
				  struct vm_area_struct, shared.rb);
		if (parent->shared.rb_subtree_last < last)
			parent->shared.rb_subtree_last = last;
		while (parent->shared.rb.rb_left) {
			parent = rb_entry(parent->shared.rb.rb_left,
				struct vm_area_struct, shared.rb);
			if (parent->shared.rb_subtree_last < last)
				parent->shared.rb_subtree_last = last;
		}
		link = &parent->shared.rb.rb_left;
	}

	node->shared.rb_subtree_last = last;
	rb_link_node(&node->shared.rb, &parent->shared.rb, link);
	rb_insert_augmented(&node->shared.rb, &root->rb_root,
			    &__vma_interval_tree_augment);
	tree->nr_vmas++;
}

static inline unsigned long avc_start_pgoff(struct anon_vma_chain *avc)
{
	return vma_start_pgoff(avc->vma);
}

static inline unsigned long avc_last_pgoff(struct anon_vma_chain *avc)
{
	return vma_last_pgoff(avc->vma);
}

INTERVAL_TREE_DEFINE(struct anon_vma_chain, rb, unsigned long, rb_subtree_last,
		     avc_start_pgoff, avc_last_pgoff,
		     static inline, __anon_vma_interval_tree)

void anon_vma_interval_tree_insert(struct anon_vma_chain *node,
				   struct rb_root_cached *root)
{
#ifdef CONFIG_DEBUG_VM_RB
	node->cached_vma_start = avc_start_pgoff(node);
	node->cached_vma_last = avc_last_pgoff(node);
#endif
	__anon_vma_interval_tree_insert(node, root);
}

void anon_vma_interval_tree_remove(struct anon_vma_chain *node,
				   struct rb_root_cached *root)
{
	__anon_vma_interval_tree_remove(node, root);
}

struct anon_vma_chain *
anon_vma_interval_tree_iter_first(struct rb_root_cached *root,
				  unsigned long first, unsigned long last)
{
	return __anon_vma_interval_tree_iter_first(root, first, last);
}

struct anon_vma_chain *
anon_vma_interval_tree_iter_next(struct anon_vma_chain *node,
				 unsigned long first, unsigned long last)
{
	return __anon_vma_interval_tree_iter_next(node, first, last);
}

#ifdef CONFIG_DEBUG_VM_RB
void anon_vma_interval_tree_verify(struct anon_vma_chain *node)
{
	WARN_ON_ONCE(node->cached_vma_start != avc_start_pgoff(node));
	WARN_ON_ONCE(node->cached_vma_last != avc_last_pgoff(node));
}
#endif
