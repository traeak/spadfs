#ifndef __SPAD__
#include "sck.h"
#else
#include "SCK.H"
#endif

int cross_link_pass;
int were_cross_links;

static XLIST_HEAD cross_links;

struct cross_link {
	LIST_ENTRY le;
	blk_t from;
	blk_t to;
};

void init_cross_links(void)
{
	cross_link_pass = 0;
	were_cross_links = 0;
	INIT_XLIST(&cross_links);
}

void done_cross_links(void)
{
	struct cross_link *cl;
	while (!XLIST_EMPTY(&cross_links)) {
		cl = LIST_STRUCT(cross_links.next, struct cross_link, le);
		DEL_FROM_LIST(&cl->le);
		mem_free(cl);
	}
}

static int add_cross_link(blk_t from, blk_t l)
{
	blk_t to = from + l;
	if (!dont_store_cross_links) {
		struct cross_link *cl;
		XLIST_FOR_EACH(cl, &cross_links, struct cross_link, le) {
			if (__unlikely(to >= cl->from) && __unlikely(from <= cl->to)) {
				if (from < cl->from) cl->from = from;
				if (to > cl->to) cl->to = to;
				goto cl_marked;
			}
		}
		if (__unlikely(!(cl = mem_alloc(sizeof(struct cross_link))))) {
			log_printf(1, "CAN'T ALLOC MEMORY FOR CROSS LINK DESCRIPTION (TRY /DONT_STORE_CROSS_LINKS)");
			return 2;
		}
		cl->from = from;
		cl->to = to;
		ADD_TO_XLIST(&cross_links, &cl->le);
		cl_marked:;
	}
	were_cross_links = 1;
	return 0;
}

static int check_cross_link(blk_t from, blk_t l)
{
	struct cross_link *cl;
	blk_t to = from + l;
	XLIST_FOR_EACH(cl, &cross_links, struct cross_link, le) {
		if (__unlikely(to > cl->from) && __unlikely(from < cl->to)) return 1;
	}
	return 0;
}

/* returns:
	0 --- ok
	1 --- space already occupied
	2 --- malloc failed
	-1 --- space already occupied, will need more passes (continue and let it be)
*/

int sys_alloc(blk_t blk, blk_t l, int pass, char *name, blk_t *off, const char *(*get_filename)(void *p), void *p)
{
	char msg[__MAX_STR_LEN];
#define print_msg	\
		if (off) _snprintf(msg, sizeof msg, "%s (LOGICAL BLOCK %"blk_format"X)", name, *off), name = msg, off = NULL;
	int r;
#if 0
	if (pass == 1 && blk >= ((blk_t)SPAD2CPU16_LV(super.small_file_group) << super.sectors_per_group_bits)) {
		printf("alloc out of zone: %llx (%llx) - %s\n", blk, ((blk_t)1 << 20) * 9, name);
	}
#endif
	r = check_allocation(blk, l);
	if (__unlikely(r)) {
		print_msg;
		if (r == 1) log_printf(0, "%s: %s ALLOCATION ERROR: %s", get_filename(p), escape_name(name), alloc_error);
		return r;
	}
	if (__unlikely(cross_link_pass) && __likely(cross_link_pass < 3) && __unlikely(cross_link_pass != pass)) return 0;
	r = alloc_blocks(blk, l);
	if (__unlikely(r)) {
		if (r == 1) {
			if (!cross_link_pass) {
				if (__unlikely(add_cross_link(blk, l))) return 2;
				return -1;
			} else {
				print_msg;
				log_printf(0, "%s: %s CROSS LINKED AT (%"blk_format"X,%"blk_format"X), CLEARING", get_filename(p), escape_name(name), blk, l);
			}
		}
		return r;
	}
	if (__unlikely(cross_link_pass) && __likely(cross_link_pass < 3) && __unlikely(check_cross_link(blk, l))) {
		print_msg;
		log_printf(0, "%s: %s CROSS LINKED AT (%"blk_format"X,%"blk_format"X), LEAVING", get_filename(p), escape_name(name), blk, l);
	}
	return 0;
}

int sys_free(blk_t blk, blk_t l, int pass)
{
	if (__likely(pass >= 0) && __unlikely(cross_link_pass) && __likely(cross_link_pass < 3) && __unlikely(cross_link_pass != pass)) return 0;
	return free_blocks(blk, l);
}
