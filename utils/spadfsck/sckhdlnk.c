#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

#define HARDLINK_HASH_SIZE	65536	/* must be power of 2 */

typedef struct {
	LIST_ENTRY hash;
	RECOVERY *recovery;
	blk_t fx_fnode;
	blk_t nlink;
	blk_t disk_nlink;
} HARDLINK;

static XLIST_HEAD hardlink_hash[HARDLINK_HASH_SIZE];
#define HASH(blk)	((unsigned)(blk) & (HARDLINK_HASH_SIZE - 1))

static HARDLINK *hardlink_find(blk_t blk)
{
	HARDLINK *h;
	unsigned hash = HASH(blk);
	XLIST_FOR_EACH(h, &hardlink_hash[hash], HARDLINK, hash)
		if (__likely(h->fx_fnode == blk)) return h;
	return NULL;
}

/* 0 --- created, 1 --- found, -1 --- found and need recovery, 2 --- out of memory */
int hardlink_link(blk_t blk)
{
	HARDLINK *h;
	if ((h = hardlink_find(blk))) {
		h->nlink++;
		if (__unlikely(h->recovery != NULL)) return -1;
		return 1;
	}
	h = mem_alloc(sizeof(HARDLINK));
	if (__unlikely(!h)) return 2;
	h->recovery = NULL;
	h->fx_fnode = blk;
	h->nlink = 1;
	h->disk_nlink = 0;
	ADD_TO_XLIST(&hardlink_hash[HASH(blk)], &h->hash);
	return 0;
}

void hardlink_unlink(blk_t blk)
{
	HARDLINK *h = hardlink_find(blk);
	if (__unlikely(!h)) panic("hardlink_unlink: hardlink for block %"blk_format"X not found", blk);
	if (__unlikely(h->nlink <= 1)) panic("hardlink_unlink: hardlink for block %"blk_format"X has nlink %"blk_format"X", blk, h->nlink);
	h->nlink--;
}

void hardlink_delete(blk_t blk)
{
	HARDLINK *h = hardlink_find(blk);
	if (__unlikely(!h)) panic("hardlink_delete: hardlink for block %"blk_format"X not found", blk);
	if (__unlikely(h->nlink != 1)) panic("hardlink_delete: hardlink for block %"blk_format"X has nlink %"blk_format"X", blk, h->nlink);
	if (h->recovery)
		return;
	DEL_FROM_LIST(&h->hash);
	mem_free(h);
}

void hardlink_set_disk_nlink(blk_t blk, blk_t disk_nlink)
{
	HARDLINK *h = hardlink_find(blk);
	if (__unlikely(!h)) panic("hardlink_set_disk_nlink: hardlink for block %"blk_format"X not found", blk);
	if (__unlikely(h->nlink != 1)) panic("hardlink_set_disk_nlink: hardlink for block %"blk_format"X has nlink %"blk_format"X", blk, h->nlink);
	if (__unlikely(h->disk_nlink != 0)) panic("hardlink_set_disk_nlink: disk_nlink for block %"blk_format"X already set: %"blk_format"X", blk, h->disk_nlink);
	h->disk_nlink = disk_nlink;
}

void hardlink_set_recovery(blk_t blk, RECOVERY *rec)
{
	HARDLINK *h = hardlink_find(blk);
	if (__unlikely(!h)) panic("hardlink_set_recovery: hardlink for block %"blk_format"X not found", blk);
	if (__unlikely(h->nlink != 1)) panic("hardlink_set_recovery: hardlink for block %"blk_format"X has nlink %"blk_format"X", blk, h->nlink);
	if (__unlikely(h->recovery != NULL)) panic("hardlink_set_recovery: hardlink for block %"blk_format"X has already recovery record", blk);
	h->recovery = rec;
}

RECOVERY *hardlink_get_recovery(blk_t blk)
{
	HARDLINK *h = hardlink_find(blk);
	if (__unlikely(!h)) panic("hardlink_get_recovery: hardlink for block %"blk_format"X not found", blk);
	if (__unlikely(h->recovery == NULL)) panic("hardlink_get_recovery: hardlink for block %"blk_format"X has no recovery", blk);
	return h->recovery;
}

int hardlink_search_for_bad_nlink(blk_t *fx_blk, blk_t *real_nlink)
{
	unsigned i;
	for (i = 0; i < HARDLINK_HASH_SIZE; i++) while (!XLIST_EMPTY(&hardlink_hash[i])) {
		HARDLINK *h = LIST_STRUCT(hardlink_hash[i].next, HARDLINK, hash);
		DEL_FROM_LIST(&h->hash);
		if (__likely(!h->recovery) && __unlikely(h->nlink != h->disk_nlink)) {
			*fx_blk = h->fx_fnode;
			*real_nlink = h->nlink;
			mem_free(h);
			return 1;
		}
		mem_free(h);
	}
	return 0;
}

void hardlink_init(void)
{
	unsigned i;
	for (i = 0; i < HARDLINK_HASH_SIZE; i++) INIT_XLIST(&hardlink_hash[i]);
}

void hardlink_done(void)
{
	unsigned i;
	for (i = 0; i < HARDLINK_HASH_SIZE; i++) while (!XLIST_EMPTY(&hardlink_hash[i])) {
		HARDLINK *h = LIST_STRUCT(hardlink_hash[i].next, HARDLINK, hash);
		DEL_FROM_LIST(&h->hash);
		mem_free(h);
	}
}
