/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Code for manipulating bucket marks for garbage collection.
 *
 * Copyright 2014 Datera, Inc.
 */

#ifndef _BUCKETS_H
#define _BUCKETS_H

#include "buckets_types.h"
#include "super.h"

#define for_each_bucket(_b, _buckets)				\
	for (_b = (_buckets)->b + (_buckets)->first_bucket;	\
	     _b < (_buckets)->b + (_buckets)->nbuckets; _b++)

#define bucket_cmpxchg(g, new, expr)				\
({								\
	struct bucket *_g = g;					\
	u64 _v = atomic64_read(&(g)->_mark.v);			\
	struct bucket_mark _old;				\
								\
	do {							\
		(new).v.counter = _old.v.counter = _v;		\
		expr;						\
	} while ((_v = atomic64_cmpxchg(&(_g)->_mark.v,		\
			       _old.v.counter,			\
			       (new).v.counter)) != _old.v.counter);\
	_old;							\
})

static inline struct bucket_array *__bucket_array(struct bch_dev *ca,
						  bool gc)
{
	return rcu_dereference_check(ca->buckets[gc],
				     !ca->fs ||
				     percpu_rwsem_is_held(&ca->fs->mark_lock) ||
				     lockdep_is_held(&ca->fs->gc_lock) ||
				     lockdep_is_held(&ca->bucket_lock));
}

static inline struct bucket_array *bucket_array(struct bch_dev *ca)
{
	return __bucket_array(ca, false);
}

static inline struct bucket *__bucket(struct bch_dev *ca, size_t b, bool gc)
{
	struct bucket_array *buckets = __bucket_array(ca, gc);

	BUG_ON(b < buckets->first_bucket || b >= buckets->nbuckets);
	return buckets->b + b;
}

static inline struct bucket *gc_bucket(struct bch_dev *ca, size_t b)
{
	return __bucket(ca, b, true);
}

static inline struct bucket *bucket(struct bch_dev *ca, size_t b)
{
	return __bucket(ca, b, false);
}

static inline struct bucket_gens *bucket_gens(struct bch_dev *ca)
{
	return rcu_dereference_check(ca->bucket_gens,
				     !ca->fs ||
				     percpu_rwsem_is_held(&ca->fs->mark_lock) ||
				     lockdep_is_held(&ca->fs->gc_lock) ||
				     lockdep_is_held(&ca->bucket_lock));

}

static inline u8 *bucket_gen(struct bch_dev *ca, size_t b)
{
	struct bucket_gens *gens = bucket_gens(ca);

	BUG_ON(b < gens->first_bucket || b >= gens->nbuckets);
	return gens->b + b;
}

/*
 * bucket_gc_gen() returns the difference between the bucket's current gen and
 * the oldest gen of any pointer into that bucket in the btree.
 */

static inline u8 bucket_gc_gen(struct bucket *g)
{
	return g->mark.gen - g->oldest_gen;
}

static inline size_t PTR_BUCKET_NR(const struct bch_dev *ca,
				   const struct bch_extent_ptr *ptr)
{
	return sector_to_bucket(ca, ptr->offset);
}

static inline struct bucket *PTR_BUCKET(struct bch_dev *ca,
					const struct bch_extent_ptr *ptr)
{
	return bucket(ca, PTR_BUCKET_NR(ca, ptr));
}

static inline struct bucket *PTR_GC_BUCKET(struct bch_dev *ca,
					   const struct bch_extent_ptr *ptr)
{
	return gc_bucket(ca, PTR_BUCKET_NR(ca, ptr));
}

static inline enum bch_data_type ptr_data_type(const struct bkey *k,
					       const struct bch_extent_ptr *ptr)
{
	if (k->type == KEY_TYPE_btree_ptr ||
	    k->type == KEY_TYPE_btree_ptr_v2)
		return BCH_DATA_btree;

	return ptr->cached ? BCH_DATA_cached : BCH_DATA_user;
}

static inline int gen_cmp(u8 a, u8 b)
{
	return (s8) (a - b);
}

static inline int gen_after(u8 a, u8 b)
{
	int r = gen_cmp(a, b);

	return r > 0 ? r : 0;
}

/**
 * ptr_stale() - check if a pointer points into a bucket that has been
 * invalidated.
 */
static inline u8 ptr_stale(struct bch_dev *ca,
			   const struct bch_extent_ptr *ptr)
{
	u8 ret;

	rcu_read_lock();
	ret = gen_after(*bucket_gen(ca, PTR_BUCKET_NR(ca, ptr)), ptr->gen);
	rcu_read_unlock();

	return ret;
}

/* bucket gc marks */

static inline bool is_available_bucket(struct bucket_mark mark)
{
	return !mark.dirty_sectors && !mark.stripe;
}

/* Device usage: */

struct bch_dev_usage bch2_dev_usage_read(struct bch_dev *);

static inline u64 __dev_buckets_available(struct bch_dev *ca,
					  struct bch_dev_usage stats)
{
	u64 total = ca->mi.nbuckets - ca->mi.first_bucket;

	if (WARN_ONCE(stats.buckets_unavailable > total,
		      "buckets_unavailable overflow (%llu > %llu)\n",
		      stats.buckets_unavailable, total))
		return 0;

	return total - stats.buckets_unavailable;
}

static inline u64 dev_buckets_available(struct bch_dev *ca)
{
	return __dev_buckets_available(ca, bch2_dev_usage_read(ca));
}

static inline u64 __dev_buckets_reclaimable(struct bch_dev *ca,
					    struct bch_dev_usage stats)
{
	struct bch_fs *c = ca->fs;
	s64 available = __dev_buckets_available(ca, stats);
	unsigned i;

	spin_lock(&c->freelist_lock);
	for (i = 0; i < RESERVE_NR; i++)
		available -= fifo_used(&ca->free[i]);
	available -= fifo_used(&ca->free_inc);
	available -= ca->nr_open_buckets;
	spin_unlock(&c->freelist_lock);

	return max(available, 0LL);
}

static inline u64 dev_buckets_reclaimable(struct bch_dev *ca)
{
	return __dev_buckets_reclaimable(ca, bch2_dev_usage_read(ca));
}

/* Filesystem usage: */

static inline unsigned fs_usage_u64s(struct bch_fs *c)
{

	return sizeof(struct bch_fs_usage) / sizeof(u64) +
		READ_ONCE(c->replicas.nr);
}

static inline unsigned dev_usage_u64s(void)
{
	return sizeof(struct bch_dev_usage) / sizeof(u64);
}

u64 bch2_fs_usage_read_one(struct bch_fs *, u64 *);

struct bch_fs_usage_online *bch2_fs_usage_read(struct bch_fs *);

void bch2_fs_usage_acc_to_base(struct bch_fs *, unsigned);

void bch2_fs_usage_to_text(struct printbuf *,
			   struct bch_fs *, struct bch_fs_usage_online *);

u64 bch2_fs_sectors_used(struct bch_fs *, struct bch_fs_usage_online *);

struct bch_fs_usage_short
bch2_fs_usage_read_short(struct bch_fs *);

/* key/bucket marking: */

void bch2_fs_usage_initialize(struct bch_fs *);

void bch2_mark_alloc_bucket(struct bch_fs *, struct bch_dev *, size_t, bool);
void bch2_mark_metadata_bucket(struct bch_fs *, struct bch_dev *,
			       size_t, enum bch_data_type, unsigned,
			       struct gc_pos, unsigned);

int bch2_mark_key(struct btree_trans *, struct bkey_s_c, struct bkey_s_c, unsigned);

int bch2_mark_update(struct btree_trans *, struct btree_path *,
		     struct bkey_i *, unsigned);

int bch2_trans_mark_key(struct btree_trans *, struct bkey_s_c,
			struct bkey_s_c, unsigned);
int bch2_trans_fs_usage_apply(struct btree_trans *, struct replicas_delta_list *);

int bch2_trans_mark_metadata_bucket(struct btree_trans *, struct bch_dev *,
				    size_t, enum bch_data_type, unsigned);
int bch2_trans_mark_dev_sb(struct bch_fs *, struct bch_dev *);

/* disk reservations: */

static inline void bch2_disk_reservation_put(struct bch_fs *c,
					     struct disk_reservation *res)
{
	this_cpu_sub(*c->online_reserved, res->sectors);
	res->sectors = 0;
}

#define BCH_DISK_RESERVATION_NOFAIL		(1 << 0)

int bch2_disk_reservation_add(struct bch_fs *,
			      struct disk_reservation *,
			      u64, int);

static inline struct disk_reservation
bch2_disk_reservation_init(struct bch_fs *c, unsigned nr_replicas)
{
	return (struct disk_reservation) {
		.sectors	= 0,
#if 0
		/* not used yet: */
		.gen		= c->capacity_gen,
#endif
		.nr_replicas	= nr_replicas,
	};
}

static inline int bch2_disk_reservation_get(struct bch_fs *c,
					    struct disk_reservation *res,
					    u64 sectors, unsigned nr_replicas,
					    int flags)
{
	*res = bch2_disk_reservation_init(c, nr_replicas);

	return bch2_disk_reservation_add(c, res, sectors * nr_replicas, flags);
}

#define RESERVE_FACTOR	6

static inline u64 avail_factor(u64 r)
{
	return div_u64(r << RESERVE_FACTOR, (1 << RESERVE_FACTOR) + 1);
}

int bch2_dev_buckets_resize(struct bch_fs *, struct bch_dev *, u64);
void bch2_dev_buckets_free(struct bch_dev *);
int bch2_dev_buckets_alloc(struct bch_fs *, struct bch_dev *);

#endif /* _BUCKETS_H */
