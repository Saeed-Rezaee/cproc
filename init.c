#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "decl.h"
#include "expr.h"
#include "init.h"
#include "pp.h"
#include "token.h"
#include "type.h"

struct object {
	uint64_t offset;
	struct type *type;
	union {
		struct member *mem;
		size_t idx;
	};
	bool iscur;
};

struct initparser {
	/* TODO: keep track of type depth, and allocate maximum possible
	   number of nested objects in initializer */
	struct object obj[32], *cur, *sub;
};

struct init *
mkinit(uint64_t start, uint64_t end, struct expr *expr)
{
	struct init *init;

	init = xmalloc(sizeof(*init));
	init->start = start;
	init->end = end;
	init->expr = expr;
	init->next = NULL;

	return init;
}

static void
initadd(struct init **init, struct init *new)
{
	struct init *old;

	for (; old = *init; init = &old->next) {
		if (new->start >= old->end)
			continue;
		/* no overlap, insert before `old` */
		if (new->end <= old->start)
			break;
		/* replace any initializers that `new` covers */
		if (new->end >= old->end) {
			do old = old->next;
			while (old && new->end >= old->end);
			break;
		}
		/* `old` covers `new`, keep looking */
	}
	new->next = old;
	*init = new;
}

static void
updatearray(struct type *t, uint64_t i)
{
	if (!t->incomplete)
		return;
	if (++i > t->array.length) {
		t->array.length = i;
		t->size = i * t->base->size;
	}
}

static void
subobj(struct initparser *p, struct type *t, uint64_t off)
{
	off += p->sub->offset;
	if (++p->sub == p->obj + LEN(p->obj))
		fatal("internal error: too many designators");
	p->sub->type = typeunqual(t, NULL);
	p->sub->offset = off;
	p->sub->iscur = false;
}

static bool
findmember(struct initparser *p, char *name)
{
	struct member *m;

	for (m = p->sub->type->structunion.members; m; m = m->next) {
		if (m->name) {
			if (strcmp(m->name, name) == 0) {
				p->sub->mem = m;
				subobj(p, m->type, m->offset);
				return true;
			}
		} else {
			subobj(p, m->type, m->offset);
			if (findmember(p, name))
				return true;
			--p->sub;
		}
	}
	return false;
}

static void
designator(struct scope *s, struct initparser *p)
{
	struct type *t;
	char *name;

	p->sub = p->cur;
	for (;;) {
		t = p->sub->type;
		switch (tok.kind) {
		case TLBRACK:
			if (t->kind != TYPEARRAY)
				error(&tok.loc, "index designator is only valid for array types");
			next();
			p->sub->idx = intconstexpr(s, false);
			if (t->incomplete)
				updatearray(t, p->sub->idx);
			else if (p->sub->idx >= t->array.length)
				error(&tok.loc, "index designator is larger than array length");
			expect(TRBRACK, "for index designator");
			subobj(p, t->base, p->sub->idx * t->base->size);
			break;
		case TPERIOD:
			if (t->kind != TYPESTRUCT && t->kind != TYPEUNION)
				error(&tok.loc, "member designator only valid for struct/union types");
			next();
			name = expect(TIDENT, "for member designator");
			if (!findmember(p, name))
				error(&tok.loc, "%s has no member named '%s'", t->kind == TYPEUNION ? "union" : "struct", name);
			free(name);
			break;
		default:
			expect(TASSIGN, "after designator");
			return;
		}
	}
}

static void
focus(struct initparser *p)
{
	struct type *t;

	switch (p->sub->type->kind) {
	case TYPEARRAY:
		p->sub->idx = 0;
		if (p->sub->type->incomplete)
			updatearray(p->sub->type, p->sub->idx);
		t = p->sub->type->base;
		break;
	case TYPESTRUCT:
	case TYPEUNION:
		p->sub->mem = p->sub->type->structunion.members;
		t = p->sub->mem->type;
		break;
	default:
		t = p->sub->type;
	}
	subobj(p, t, 0);
}

static void
advance(struct initparser *p)
{
	struct type *t;

	for (;;) {
		--p->sub;
		t = p->sub->type;
		switch (t->kind) {
		case TYPEARRAY:
			++p->sub->idx;
			if (t->incomplete)
				updatearray(t, p->sub->idx);
			if (p->sub->idx < t->array.length) {
				subobj(p, t->base, t->base->size * p->sub->idx);
				return;
			}
			break;
		case TYPESTRUCT:
			p->sub->mem = p->sub->mem->next;
			if (p->sub->mem) {
				subobj(p, p->sub->mem->type, p->sub->mem->offset);
				return;
			}
			break;
		}
		if (p->sub == p->cur)
			error(&tok.loc, "too many initializers for type");
	}
}

/* 6.7.9 Initialization */
struct init *
parseinit(struct scope *s, struct type *t)
{
	struct initparser p;
	struct init *init = NULL;
	struct expr *expr;
	struct type *base;

	t = typeunqual(t, NULL);
	p.cur = NULL;
	p.sub = p.obj;
	p.sub->offset = 0;
	p.sub->type = t;
	p.sub->iscur = false;
	if (t->incomplete && !(t->kind == TYPEARRAY && t->array.length == 0))
		error(&tok.loc, "initializer specified for incomplete type");
	for (;;) {
		if (p.cur) {
			if (tok.kind == TLBRACK || tok.kind == TPERIOD)
				designator(s, &p);
			else if (p.sub != p.cur)
				advance(&p);
			else
				focus(&p);
		}
		if (tok.kind == TLBRACE) {
			next();
			if (p.cur && p.cur->type == p.sub->type)
				error(&tok.loc, "nested braces around scalar initializer");
			p.cur = p.sub;
			p.cur->iscur = true;
			continue;
		}
		expr = assignexpr(s);
		for (;;) {
			t = p.sub->type;
			switch (t->kind) {
			case TYPEARRAY:
				if (expr->flags & EXPRFLAG_DECAYED && expr->unary.base->kind == EXPRSTRING) {
					expr = expr->unary.base;
					base = typeunqual(t->base, NULL);
					/* XXX: wide string literals */
					if (!(typeprop(base) & PROPCHAR))
						error(&tok.loc, "array initializer is string literal with incompatible type");
					if (t->incomplete)
						updatearray(t, expr->string.size);
					goto add;
				}
				break;
			case TYPESTRUCT:
			case TYPEUNION:
				if (typecompatible(expr->type, t))
					goto add;
				break;
			default:  /* scalar type */
				assert(typeprop(t) & PROPSCALAR);
				expr = exprconvert(expr, t);
				goto add;
			}
			focus(&p);
		}
	add:
		initadd(&init, mkinit(p.sub->offset, p.sub->offset + p.sub->type->size, expr));
		for (;;) {
			if (p.sub->type->kind == TYPEARRAY && p.sub->type->incomplete)
				p.sub->type->incomplete = false;
			if (!p.cur)
				return init;
			if (tok.kind == TCOMMA) {
				next();
				if (tok.kind != TRBRACE)
					break;
			} else if (tok.kind != TRBRACE) {
				error(&tok.loc, "expected ',' or '}' after initializer");
			}
			next();
			p.sub = p.cur;
			do p.cur = p.cur == p.obj ? NULL : p.cur - 1;
			while (p.cur && !p.cur->iscur);
		}
	}
}
