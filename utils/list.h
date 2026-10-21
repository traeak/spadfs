#ifndef __SPAD_LIST_H
#define __SPAD_LIST_H

#ifdef __SPAD__
#include <ARCH/BARRIER.H>
#include <SYS/TYPES.H>
#include <ARCH/SETUP.H>
#endif

typedef struct __list_head LIST_HEAD;
typedef struct __xlist_head XLIST_HEAD;
typedef struct __list_entry LIST_ENTRY;

struct __list_head {
	LIST_ENTRY *next, *prev;
};

struct __xlist_head {
	LIST_ENTRY *next;
};

struct __list_entry {
	LIST_ENTRY *next, *prev;
};

extern LIST_ENTRY LIST_END;

static __finline__ void __LIST_ENTRY_TEST_TYPE(LIST_ENTRY *h) { h = h + 0; }
static __finline__ void __LIST_HEAD_TEST_TYPE(LIST_HEAD *h) { h = h + 0; }
static __finline__ void __XLIST_HEAD_TEST_TYPE(XLIST_HEAD *h) { h = h + 0; }

static __finline__ void ADD_TO_LIST(LIST_HEAD *h, LIST_ENTRY *e)
{
	e->prev = (LIST_ENTRY *)(void *)h;
	e->next = h->next;
	h->next->prev = e;
	h->next = e;
}

static __finline__ void ADD_TO_LIST_END(LIST_HEAD *h, LIST_ENTRY *e)
{
	e->next = (LIST_ENTRY *)(void *)h;
	e->prev = h->prev;
	h->prev->next = e;
	h->prev = e;
}

static __finline__ void ADD_TO_LIST_AFTER(LIST_ENTRY *h, LIST_ENTRY *e)
{
	ADD_TO_LIST((LIST_HEAD *)(void *)h, e);
}

static __finline__ void ADD_TO_LIST_BEFORE(LIST_ENTRY *h, LIST_ENTRY *e)
{
	ADD_TO_LIST_END((LIST_HEAD *)(void *)h, e);
}

static __finline__ void ADD_TO_XLIST(XLIST_HEAD *h, LIST_ENTRY *e)
{
	e->prev = (LIST_ENTRY *)(void *)h;
	e->next = h->next;
	h->next->prev = e;
	h->next = e;
}

static __finline__ void ADD_TO_XLIST_ORDERED(XLIST_HEAD *h, LIST_ENTRY *e)
{
	e->prev = (LIST_ENTRY *)(void *)h;
	e->next = h->next;
	__barrier();
	h->next->prev = e;
	h->next = e;
}

static __finline__ void DEL_FROM_LIST(LIST_ENTRY *e)
{
	e->prev->next = e->next;	/* this may alias the head */
	e->next->prev = e->prev;	/* this may alias the head */
#if __DEBUG >= 2
	e->prev = e->next = NULL;
#endif
}

static __finline__ void DEL_FROM_LIST_ORDERED(LIST_ENTRY *e)
{
	e->prev->next = e->next;
	e->next->prev = e->prev;
	__barrier();
#if __DEBUG >= 2
	e->prev = e->next = NULL;	/* !!! SMPFIX: take care of the other cpu reading this */
#endif
}

static __finline__ void VOID_LIST_ENTRY(LIST_ENTRY *h)
{
	h->next = h->prev = h;
}

static __finline__ void INIT_LIST(LIST_HEAD *h)
{
	h->next = h->prev = (LIST_ENTRY *)(void *)h;
}

static __finline__ void INIT_XLIST(XLIST_HEAD *h)
{
	h->next = (LIST_ENTRY *)(void *)&LIST_END;
}

#define DECL_LIST(h) LIST_HEAD h = { (LIST_ENTRY *)(void *)&(h), (LIST_ENTRY *)(void *)&(h) }

#define DECL_XLIST(h) XLIST_HEAD h = { (LIST_ENTRY *)(void *)&LIST_END }

#define INIT_VAL_LIST(h) { (LIST_ENTRY *)(void *)&(h), (LIST_ENTRY *)(void *)&(h) }

#define INIT_VAL_XLIST(h) { (LIST_ENTRY *)(void *)&LIST_END }

#ifdef LIST_EMPTY
#undef LIST_EMPTY
#endif

#define LIST_VOID(h)	(__LIST_ENTRY_TEST_TYPE(h), (h) == (h)->next)
#define LIST_EMPTY(h)	(__LIST_HEAD_TEST_TYPE(h), (LIST_ENTRY *)(void *)(h) == (h)->next)
#define XLIST_EMPTY(h)	(__XLIST_HEAD_TEST_TYPE(h), (h)->next == (LIST_ENTRY *)(void *)&LIST_END)

static __finline__ void LIST_JOIN(LIST_HEAD *to, LIST_HEAD *list)
{
	LIST_ENTRY *f = list->next;

	if (f != (LIST_ENTRY *)list) {
		LIST_ENTRY *l, *a;

		l = list->prev;
		a = to->next;

		f->prev = (LIST_ENTRY *)(void *)to;
		to->next = f;

		l->next = a;	/* this may alias the head */
		a->prev = l;	/* this may alias the head */
	}
}

static __finline__ void LIST_JOIN_END(LIST_HEAD *to, LIST_HEAD *list)
{
	LIST_ENTRY *f = list->prev;

	if (f != (LIST_ENTRY *)list) {
		LIST_ENTRY *l, *a;

		l = list->next;
		a = to->prev;

		f->next = (LIST_ENTRY *)(void *)to;
		to->prev = f;

		l->prev = a;	/* this may alias the head */
		a->next = l;	/* this may alias the head */
	}
}

static __finline__ void MOVE_LIST_HEAD(LIST_HEAD *to, LIST_HEAD *from)
{
	if (__likely(!LIST_EMPTY(from))) {
		(to->next = from->next)->prev = (LIST_ENTRY *)(void *)to;
		(to->prev = from->prev)->next = (LIST_ENTRY *)(void *)to;
	} else {
		INIT_LIST(to);
	}
#if __DEBUG >= 2
	from->prev = from->next = NULL;
#endif
}

static __finline__ void MOVE_XLIST_HEAD(XLIST_HEAD *to, XLIST_HEAD *from)
{
	(to->next = from->next)->prev = (LIST_ENTRY *)(void *)to;
#if __DEBUG >= 2
	from->next = NULL;
#endif
}

#define __LIST_STRUCT_NOCHECK(ptr, type, member) \
	((type *)(void *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))

#define LIST_STRUCT(ptr, type, member) \
	(__LIST_ENTRY_TEST_TYPE(ptr), __LIST_STRUCT_NOCHECK(ptr, type, member))

#define __LIST_FOR_EACH(v, head, type, member, direction, condition) \
	for ((v) = __LIST_STRUCT_NOCHECK((head)->direction, type, member), __LIST_HEAD_TEST_TYPE(head); condition((v) != __LIST_STRUCT_NOCHECK((head), type, member)); (v) = LIST_STRUCT((v)->member.direction, type, member))

#define LIST_FOR_EACH(v, head, type, member) __LIST_FOR_EACH(v, head, type, member, next, )
#define LIST_FOR_EACH_BACK(v, head, type, member) __LIST_FOR_EACH(v, head, type, member, prev, )
#define LIST_FOR_EACH_UNLIKELY(v, head, type, member) __LIST_FOR_EACH(v, head, type, member, next, __unlikely)
#define LIST_FOR_EACH_BACK_UNLIKELY(v, head, type, member) __LIST_FOR_EACH(v, head, type, member, prev, __unlikely)

#define __XLIST_FOR_EACH(v, head, type, member, condition) \
	for ((v) = __LIST_STRUCT_NOCHECK((head)->next, type, member), __XLIST_HEAD_TEST_TYPE(head); condition((v) != __LIST_STRUCT_NOCHECK(&LIST_END, type, member)); (v) = LIST_STRUCT((v)->member.next, type, member))

#define XLIST_FOR_EACH(v, head, type, member) __XLIST_FOR_EACH(v, head, type, member, )

#define XLIST_FOR_EACH_UNLIKELY(v, head, type, member) __XLIST_FOR_EACH(v, head, type, member, __unlikely)

#endif
