/*-
 * Copyright (C) 2016-2018  Dridi Boukelmoune
 * All rights reserved.
 *
 * Author: Dridi Boukelmoune <dridi.boukelmoune@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cache/cache.h>

#ifndef VRT_H_INCLUDED
#  include <vrt.h>
#endif

#ifndef VDEF_H_INCLUDED
#  include <vdef.h>
#endif

#include <vcl.h>
#include <vre.h>
#include <vsb.h>

#include "vcc_querystring_if.h"

/* End Of Query Parameter */
#define EOQP(c) (c == '\0' || c == '&')

/***********************************************************************
 * Type definitions
 */

struct qs_param {
	const char	*val;
	size_t		val_len;
	size_t		cmp_len;
};

struct qs_filter;

typedef int qs_match_f(VRT_CTX, const struct qs_filter *, const char *,
    unsigned);

typedef void qs_free_f(void *);

struct qs_filter {
	unsigned		magic;
#define QS_FILTER_MAGIC		0xfc750864
	union {
		void		*ptr;
		const char	*str;
	};
	qs_match_f		*match;
	qs_free_f		*free;
	VTAILQ_ENTRY(qs_filter)	list;
};

struct vmod_querystring_filter {
	unsigned			magic;
#define VMOD_QUERYSTRING_FILTER_MAGIC	0xbe8ecdb4
	VTAILQ_HEAD(, qs_filter)	filters;
	unsigned			sort;
	unsigned			uniq;
	unsigned			match_name;
};

/***********************************************************************
 * Static data structures
 */

static struct vmod_querystring_filter qs_clean_filter = {
	.magic = VMOD_QUERYSTRING_FILTER_MAGIC,
	.sort = 0,
};

static struct vmod_querystring_filter qs_sort_filter = {
	.magic = VMOD_QUERYSTRING_FILTER_MAGIC,
	.sort = 1,
};

static struct vmod_querystring_filter qs_sort_uniq_filter = {
	.magic = VMOD_QUERYSTRING_FILTER_MAGIC,
	.sort = 1,
	.uniq = 1,
};

/***********************************************************************
 * VMOD implementation
 */

int qs_cmp(const void *, const void *);

static const char *
qs_truncate(struct ws *ws, const char * const url, const char *qs)
{
	size_t len, res;
	char *str;

	CHECK_OBJ_NOTNULL(ws, WS_MAGIC);
	AN(url);
	AN(qs);
	assert(url <= qs);

	len = qs - url;
	if (len == 0)
		return ("");

	res = WS_Reserve(ws, 0);
	if (res < len + 1) {
		WS_Release(ws, 0);
		return (url);
	}

	str = ws->f;
	(void)memcpy(str, url, len);
	str[len] = '\0';
	WS_Release(ws, len + 1);
	return (str);
}

static int
qs_empty(struct ws *ws, const char * const url, const char **res)
{
	const char *qs;

	CHECK_OBJ_NOTNULL(ws, WS_MAGIC);
	AN(res);

	*res = url;

	if (url == NULL)
		return (1);

	qs = strchr(url, '?');
	if (qs == NULL)
		return (1);

	if (qs[1] == '\0') {
		*res = qs_truncate(ws, url, qs);
		return (1);
	}

	*res = qs;
	return (0);
}

static int
qs_match_string(VRT_CTX, const struct qs_filter *qsf, const char *s,
    unsigned keep)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(qsf, QS_FILTER_MAGIC);

	(void)keep;
	return (!strcmp(s, qsf->str));
}

static int
qs_match_regex(VRT_CTX, const struct qs_filter *qsf, const char *s,
    unsigned keep)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(qsf, QS_FILTER_MAGIC);

	(void)keep;
	return (VRT_re_match(ctx, s, qsf->ptr));
}

static int
qs_match_glob(VRT_CTX, const struct qs_filter *qsf, const char *s,
    unsigned keep)
{
	int match;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(qsf, QS_FILTER_MAGIC);

	match = fnmatch(qsf->str, s, 0);

	if (match == 0)
		return (1);

	if (match == FNM_NOMATCH)
		return (0);

	/* NB: If the fnmatch failed because of a wrong pattern, the error is
	 * logged but the query-string is kept intact.
	 */
	VSLb(ctx->vsl, SLT_Error, "querystring: failed to match glob `%s'",
	    qsf->str);
	return (keep);
}

int
qs_cmp(const void *v1, const void *v2)
{
	const struct qs_param *p1, *p2;
	size_t len;
	int cmp;

	AN(v1);
	AN(v2);
	p1 = v1;
	p2 = v2;

	len = p1->cmp_len < p2->cmp_len ? p1->cmp_len : p2->cmp_len;
	cmp = strncmp(p1->val, p2->val, len);

	if (cmp || p1->cmp_len == p2->cmp_len)
		return (cmp);
	return (p1->cmp_len - p2->cmp_len);
}

static unsigned
qs_match(VRT_CTX, const struct vmod_querystring_filter *obj,
    const struct qs_param *param, unsigned keep)
{
	struct qs_filter *qsf;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(obj, VMOD_QUERYSTRING_FILTER_MAGIC);

	if (param->cmp_len == 0)
		return (0);

	if (VTAILQ_EMPTY(&obj->filters))
		return (1);

	VTAILQ_FOREACH(qsf, &obj->filters, list) {
		CHECK_OBJ_NOTNULL(qsf, QS_FILTER_MAGIC);
		if (qsf->match(ctx, qsf, param->val, keep))
			return (keep);
	}

	return (!keep);
}

static size_t
qs_search(struct qs_param *key, struct qs_param *src, size_t cnt)
{
	size_t i, l = 0, u = cnt;
	int cmp;

	/* bsearch a position */
	do {
		i = (l + u) / 2;
		cmp = qs_cmp(key, src + i);
		if (cmp < 0)
			u = i;
		if (cmp > 0)
			l = i + 1;
	} while (l < u && cmp);

	/* ensure a stable sort */
	while (cmp >= 0 && ++i < cnt)
		cmp = qs_cmp(key, src + i);

	return (i);
}

static ssize_t
qs_insert(struct qs_param *new, struct qs_param *params, size_t cnt,
    unsigned sort, unsigned uniq)
{
	size_t pos = cnt;

	if (sort && cnt > 0)
		pos = qs_search(new, params, cnt);

	if (uniq && pos > 0 && !qs_cmp(new, params + pos - 1))
		return (-1);

	if (pos != cnt) {
		assert(pos < cnt);
		new = params + pos;
		(void)memmove(new + 1, new, (cnt - pos) * sizeof *new);
	}

	return (pos);
}

static char *
qs_append(char *cur, size_t cnt, struct qs_param *head)
{
	char sep;

	AN(cur);
	AN(cnt);
	AN(head);

	sep = '?';
	while (cnt > 0) {
		AZ(*cur);
		*cur = sep;
		cur++;
		(void)snprintf(cur, head->val_len + 1, "%s", head->val);
		sep = '&';
		cur += head->val_len;
		head++;
		cnt--;
	}

	return (cur);
}

static const char *
qs_apply(VRT_CTX, const char * const url, const char *qs, unsigned keep,
    const struct vmod_querystring_filter *obj)
{
	struct qs_param *params, *p, tmp;
	const char *nm, *eq;
	char *res, *cur;
	size_t len, cnt;
	ssize_t pos;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->ws, WS_MAGIC);
	CHECK_OBJ_NOTNULL(obj, VMOD_QUERYSTRING_FILTER_MAGIC);
	AN(url);
	AN(qs);
	assert(url <= qs);
	assert(*qs == '?');

	(void)WS_Reserve(ctx->ws, 0);
	res = ctx->ws->f;
	len = strlen(url) + 1;

	params = (void *)PRNDUP(res + len);
	p = params;
	if ((uintptr_t)p > (uintptr_t)ctx->ws->e) {
		WS_Release(ctx->ws, 0);
		return (url);
	}

	len = qs - url;
	(void)memcpy(res, url, len);
	cur = res + len;

	cnt = 0;
	qs++;
	AN(*qs);

	/* NB: during the matching phase we can use the preallocated space for
	 * the result's query-string in order to copy the current parameter in
	 * the loop. This saves an allocation in matchers that require a null-
	 * terminated string (e.g. glob and regex).
	 */
	tmp.val = cur;

	while (*qs != '\0') {
		nm = qs;
		eq = NULL;

		while (!EOQP(*qs)) {
			if (eq == NULL && *qs == '=')
				eq = qs;
			qs++;
		}

		if (eq == nm)
			tmp.cmp_len = 0;
		else if (obj->match_name && eq != NULL)
			tmp.cmp_len = eq - nm;
		else
			tmp.cmp_len = qs - nm;

		/* NB: reminder, tmp.val == cur */
		(void)memcpy(cur, nm, tmp.cmp_len);
		cur[tmp.cmp_len] = '\0';
		tmp.val_len = qs - nm;

		pos = -1;
		if (qs_match(ctx, obj, &tmp, keep)) {
			AN(tmp.cmp_len);
			p = params + cnt;
			if ((uintptr_t)(p + 1) > (uintptr_t)ctx->ws->e) {
				WS_Release(ctx->ws, 0);
				return (url);
			}
			pos = qs_insert(&tmp, params, cnt, obj->sort,
			    obj->uniq);
		}

		if (pos >= 0) {
			p = params + pos;
			p->val = nm;
			p->val_len = tmp.val_len;
			p->cmp_len = tmp.cmp_len;
			cnt++;
		}

		if (*qs == '&')
			qs++;
	}

	*cur = '\0';
	if (cnt > 0)
		cur = qs_append(cur, cnt, params);

	AZ(*cur);
	cur = (char *)PRNDUP(cur + 1);
	assert((uintptr_t)cur <= (uintptr_t)params);

	WS_Release(ctx->ws, cur - res);
	return (res);
}

/***********************************************************************
 * VMOD interfaces
 */

VCL_STRING
vmod_remove(VRT_CTX, VCL_STRING url)
{
	const char *res, *qs;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->ws, WS_MAGIC);

	res = NULL;
	if (qs_empty(ctx->ws, url, &res))
		return (res);

	qs = res;
	return (qs_truncate(ctx->ws, url, qs));
}

VCL_VOID
vmod_filter__init(VRT_CTX, struct vmod_querystring_filter **objp,
    const char *vcl_name, VCL_BOOL sort, VCL_BOOL uniq, VCL_STRING match)
{
	struct vmod_querystring_filter *obj;

	ASSERT_CLI();
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(objp);
	AZ(*objp);
	AN(vcl_name);

	ALLOC_OBJ(obj, VMOD_QUERYSTRING_FILTER_MAGIC);
	AN(obj);

	VTAILQ_INIT(&obj->filters);
	obj->sort = sort;
	obj->uniq = uniq;

	if (!strcmp(match, "name"))
		obj->match_name = 1;
	else if (strcmp(match, "param"))
		WRONG("Unknown matching type");

	*objp = obj;
}

VCL_VOID
vmod_filter__fini(struct vmod_querystring_filter **objp)
{
	struct vmod_querystring_filter *obj;
	struct qs_filter *qsf, *tmp;

	ASSERT_CLI();
	TAKE_OBJ_NOTNULL(obj, objp, VMOD_QUERYSTRING_FILTER_MAGIC);

	VTAILQ_FOREACH_SAFE(qsf, &obj->filters, list, tmp) {
		CHECK_OBJ_NOTNULL(qsf, QS_FILTER_MAGIC);
		if (qsf->free != NULL)
			qsf->free(qsf->ptr);
		VTAILQ_REMOVE(&obj->filters, qsf, list);
		FREE_OBJ(qsf);
	}

	FREE_OBJ(obj);
}

VCL_VOID
vmod_filter_add_string(VRT_CTX, struct vmod_querystring_filter *obj,
    VCL_STRING string)
{
	struct qs_filter *qsf;

	ASSERT_CLI();
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(obj, VMOD_QUERYSTRING_FILTER_MAGIC);
	AN(string);

	ALLOC_OBJ(qsf, QS_FILTER_MAGIC);
	AN(qsf);

	qsf->str = string;
	qsf->match = qs_match_string;
	VTAILQ_INSERT_TAIL(&obj->filters, qsf, list);
}

VCL_VOID
vmod_filter_add_glob(VRT_CTX, struct vmod_querystring_filter *obj,
    VCL_STRING glob)
{
	struct qs_filter *qsf;

	ASSERT_CLI();
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(obj, VMOD_QUERYSTRING_FILTER_MAGIC);
	AN(glob);

	ALLOC_OBJ(qsf, QS_FILTER_MAGIC);
	AN(qsf);

	qsf->str = glob;
	qsf->match = qs_match_glob;
	VTAILQ_INSERT_TAIL(&obj->filters, qsf, list);
}

VCL_VOID
vmod_filter_add_regex(VRT_CTX, struct vmod_querystring_filter *obj,
    VCL_STRING regex)
{
	struct qs_filter *qsf;
	const char *error;
	int error_offset;

	ASSERT_CLI();
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(obj, VMOD_QUERYSTRING_FILTER_MAGIC);
	AN(regex);

	ALLOC_OBJ(qsf, QS_FILTER_MAGIC);
	AN(qsf);

	qsf->ptr = VRE_compile(regex, 0, &error, &error_offset);
	if (qsf->ptr == NULL) {
		AN(ctx->msg);
		FREE_OBJ(qsf);
		VSB_printf(ctx->msg,
		    "vmod-querystring: regex error (%s): '%s' pos %d\n",
		    error, regex, error_offset);
		VRT_handling(ctx, VCL_RET_FAIL);
		return;
	}

	qsf->match = qs_match_regex;
	qsf->free = VRT_re_fini;
	VTAILQ_INSERT_TAIL(&obj->filters, qsf, list);
}

VCL_STRING
vmod_filter_apply(VRT_CTX, struct vmod_querystring_filter *obj,
    VCL_STRING url, VCL_ENUM mode)
{
	const char *tmp, *qs;
	unsigned keep;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(obj, VMOD_QUERYSTRING_FILTER_MAGIC);
	AN(mode);

	tmp = NULL;
	if (qs_empty(ctx->ws, url, &tmp))
		return (tmp);

	qs = tmp;
	keep = 0;

	if (!strcmp(mode, "keep"))
		keep = 1;
	else if (strcmp(mode, "drop"))
		WRONG("Unknown filtering mode");

	return (qs_apply(ctx, url, qs, keep, obj));
}

VCL_STRING
vmod_filter_extract(VRT_CTX, struct vmod_querystring_filter *obj,
    VCL_STRING url, VCL_ENUM mode)
{
	const char *res, *qs;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(obj, VMOD_QUERYSTRING_FILTER_MAGIC);
	AN(mode);

	if (url == NULL)
		return (NULL);

	qs = strchr(url, '?');
	if (qs == NULL || qs[1] == '\0')
		return (NULL);

	res = vmod_filter_apply(ctx, obj, qs, mode);
	AN(res);
	if (*res == '?')
		res++;
	else
		AZ(*res);
	return (res);
}

VCL_STRING
vmod_clean(VRT_CTX, VCL_STRING url)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	return (vmod_filter_apply(ctx, &qs_clean_filter, url, "keep"));
}

VCL_STRING
vmod_sort(VRT_CTX, VCL_STRING url, VCL_BOOL uniq)
{
	struct vmod_querystring_filter *filter;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	filter = uniq ? &qs_sort_uniq_filter : &qs_sort_filter;
	return (vmod_filter_apply(ctx, filter, url, "keep"));
}
