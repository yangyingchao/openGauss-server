/* -------------------------------------------------------------------------
 *
 * tsrank.c
 *		rank tsvector by tsquery
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsrank.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include <math.h>

#include "tsearch/ts_utils.h"
#include "utils/array.h"
#include "miscadmin.h"

static float weights[] = {0.1f, 0.2f, 0.4f, 1.0f};

#define wpos(wep) (w[WEP_GETWEIGHT(wep)])

#define RANK_NO_NORM 0x00
#define RANK_NORM_LOGLENGTH 0x01
#define RANK_NORM_LENGTH 0x02
#define RANK_NORM_EXTDIST 0x04
#define RANK_NORM_UNIQ 0x08
#define RANK_NORM_LOGUNIQ 0x10
#define RANK_NORM_RDIVRPLUS1 0x20
#define DEF_NORM_METHOD RANK_NO_NORM

static float calc_rank_or(const float* w, TSVector t, TSQuery q);
static float calc_rank_and(const float* w, TSVector t, TSQuery q);

/*
 * Returns a weight of a word collocation
 */
static float4 word_distance(int4 w)
{
    if (w > 100) {
        return 1e-30f;
    }

    return 1.0 / (1.005 + 0.05 * exp(((float4)w) / 1.5 - 2));
}

static int cnt_length(TSVector t)
{
    WordEntry *ptr = ARRPTR(t);
    WordEntry *end = (WordEntry*)STRPTR(t);
    int len = 0;

    while (ptr < end) {
        int clen = POSDATALEN(t, ptr);
        if (clen == 0) {
            len += 1;
        } else {
            len += clen;
        }
        ptr++;
    }

    return len;
}

#define WordECompareQueryItem(e, q, p, i, m) \
    tsCompareString((q) + (i)->distance, (i)->length, (e) + (p)->pos, (p)->len, (m))

/*
 * Returns a pointer to a WordEntry's array corresponding to 'item' from
 * tsvector 't'. 'q' is the TSQuery containing 'item'.
 * Returns NULL if not found.
 */
static WordEntry* find_wordentry(TSVector t, TSQuery q, QueryOperand* item, int32* nitem)
{
    WordEntry* stop_low = ARRPTR(t);
    WordEntry* stop_high = (WordEntry*)STRPTR(t);
    WordEntry* stop_middle = stop_high;
    int difference;

    *nitem = 0;

    /* Loop invariant: stop_low <= item < stop_high */
    while (stop_low < stop_high) {
        stop_middle = stop_low + (stop_high - stop_low) / 2;
        difference = WordECompareQueryItem(STRPTR(t), GETOPERAND(q), stop_middle, item, false);
        if (difference == 0) {
            stop_high = stop_middle;
            *nitem = 1;
            break;
        } else if (difference > 0) {
            stop_low = stop_middle + 1;
        } else {
            stop_high = stop_middle;
        }
    }

    if (item->prefix) {
        if (stop_low >= stop_high) {
            stop_middle = stop_high;
        }
        *nitem = 0;
        while (stop_middle < (WordEntry*)STRPTR(t) &&
               WordECompareQueryItem(STRPTR(t), GETOPERAND(q), stop_middle, item, true) == 0) {
            (*nitem)++;
            stop_middle++;
        }
    }

    return (*nitem > 0) ? stop_high : NULL;
}

/*
 * sort QueryOperands by (length, word)
 */
static int compare_query_operand(const void* a, const void* b, void* arg)
{
    char* operand = (char*)arg;
    QueryOperand* qa = (*(QueryOperand* const*)a);
    QueryOperand* qb = (*(QueryOperand* const*)b);

    return tsCompareString(operand + qa->distance, qa->length, operand + qb->distance, qb->length, false);
}

/*
 * Returns a sorted, de-duplicated array of QueryOperands in a query.
 * The returned QueryOperands are pointers to the original QueryOperands
 * in the query.
 *
 * Length of the returned array is stored in *size
 */
static QueryOperand** sort_and_uniq_items(TSQuery q, int* size)
{
    char* operand = GETOPERAND(q);
    QueryItem* item = GETQUERY(q);
    QueryOperand **res, **ptr, **prevptr;

    ptr = res = (QueryOperand**)palloc(sizeof(QueryOperand*) * *size);
    /* Collect all operands from the tree to res */
    while ((*size)--) {
        if (item->type == QI_VAL) {
            *ptr = (QueryOperand*)item;
            ptr++;
        }
        item++;
    }

    *size = ptr - res;
    if (*size < 2) {
        return res;
    }

    qsort_arg(res, *size, sizeof(QueryOperand*), compare_query_operand, (void*)operand);
    ptr = res + 1;
    prevptr = res;
    /* remove duplicates */
    while (ptr - res < *size) {
        if (compare_query_operand((void*)ptr, (void*)prevptr, (void*)operand) != 0) {
            prevptr++;
            *prevptr = *ptr;
        }
        ptr++;
    }

    *size = prevptr + 1 - res;
    return res;
}

static float calc_rank_and(const float* w, TSVector t, TSQuery q)
{
    WordEntryPosVector** pos = NULL;
    int i, k, l, p;
    WordEntry* entry = NULL;
    WordEntry* firstentry = NULL;
    WordEntryPos* post = NULL;
    WordEntryPos* ct = NULL;
    int4 dimt, lenct, dist, nitem;
    float res = -1.0;
    QueryOperand** item = NULL;
    int size = q->size;

    /* A dummy WordEntryPos array to use when haspos is false */
    WordEntryPosVector* pos_null_ptr = (WordEntryPosVector*)palloc(sizeof(WordEntryPosVector) + sizeof(WordEntryPos));
    pos_null_ptr->npos = 1;
    pos_null_ptr->pos[0] = 0;

    item = sort_and_uniq_items(q, &size);
    if (size < 2) {
        pfree_ext(item);
        return calc_rank_or(w, t, q);
    }
    pos = (WordEntryPosVector**)palloc0(sizeof(WordEntryPosVector*) * q->size);
    WEP_SETPOS(pos_null_ptr->pos[0], MAXENTRYPOS - 1);

    for (i = 0; i < size; i++) {
        firstentry = entry = find_wordentry(t, q, item[i], &nitem);
        if (entry == NULL) {
            continue;
        }

        while (entry - firstentry < nitem) {
            if (entry->haspos) {
                pos[i] = _POSVECPTR(t, entry);
            } else {
                pos[i] = pos_null_ptr;
            }

            dimt = pos[i]->npos;
            post = pos[i]->pos;
            for (k = 0; k < i; k++) {
                if (!pos[k]) {
                    continue;
                }
                lenct = pos[k]->npos;
                ct = pos[k]->pos;
                for (l = 0; l < dimt; l++) {
                    for (p = 0; p < lenct; p++) {
                        dist = Abs((int)WEP_GETPOS(post[l]) - (int)WEP_GETPOS(ct[p]));
                        if (dist || (dist == 0 && (pos[i] == pos_null_ptr || pos[k] == pos_null_ptr))) {
                            float curw;

                            if (!dist) {
                                dist = MAXENTRYPOS;
                            }
                            curw = sqrt(wpos(post[l]) * wpos(ct[p]) * word_distance(dist));
                            res = (res < 0) ? curw : 1.0 - (1.0 - res) * (1.0 - curw);
                        }
                    }
                }
            }
            entry++;
        }
    }
    pfree_ext(pos);
    pfree_ext(item);
    pfree_ext(pos_null_ptr);
    return res;
}

static float calc_rank_or(const float* w, TSVector t, TSQuery q)
{
    WordEntry* entry = NULL;
    WordEntry* firstentry = NULL;
    WordEntryPos* post = NULL;
    int4 dimt, j, i, nitem;
    float res = 0.0;
    QueryOperand** item;
    int size = q->size;

    /* A dummy WordEntryPos array to use when haspos is false */
    WordEntryPosVector* pos_null_ptr = (WordEntryPosVector*)palloc(sizeof(WordEntryPosVector) + sizeof(WordEntryPos));
    pos_null_ptr->npos = 1;
    pos_null_ptr->pos[0] = 0;

    item = sort_and_uniq_items(q, &size);
    for (i = 0; i < size; i++) {
        float resj, wjm;
        int4 jm;

        firstentry = entry = find_wordentry(t, q, item[i], &nitem);
        if (entry == NULL) {
            continue;
        }

        while (entry - firstentry < nitem) {
            if (entry->haspos) {
                dimt = POSDATALEN(t, entry);
                post = POSDATAPTR(t, entry);
            } else {
                dimt = pos_null_ptr->npos;
                post = pos_null_ptr->pos;
            }

            resj = 0.0;
            wjm = -1.0;
            jm = 0;
            for (j = 0; j < dimt; j++) {
                resj = resj + wpos(post[j]) / ((j + 1) * (j + 1));
                if (wpos(post[j]) > wjm) {
                    wjm = wpos(post[j]);
                    jm = j;
                }
            }
            /*
                limit (sum(i/i^2),i->inf) = pi^2/6
                resj = sum(wi/i^2),i=1,noccurence,
                wi - should be sorted desc,
                don't sort for now, just choose maximum weight. This should be corrected
                Oleg Bartunov
            */
            res = res + (wjm + resj - wjm / ((jm + 1) * (jm + 1))) / 1.64493406685;
            entry++;
        }
    }
    if (size > 0) {
        res = res / size;
    }
    pfree_ext(pos_null_ptr);
    pfree_ext(item);
    return res;
}

static float calc_rank(const float* w, TSVector t, TSQuery q, int4 method)
{
    QueryItem* item = GETQUERY(q);
    float res = 0.0;
    int len;

    if (!t->size || !q->size) {
        return 0.0;
    }

    /* XXX: What about NOT? */
    res = (item->type == QI_OPR && item->qoperator.oper == OP_AND) ? calc_rank_and(w, t, q) : calc_rank_or(w, t, q);
    if (res < 0) {
        res = 1e-20f;
    }

    if (((unsigned int4)method & RANK_NORM_LOGLENGTH) && t->size > 0) {
        res /= log((double)(cnt_length(t) + 1)) / log(2.0);
    }

    if ((unsigned int4)method & RANK_NORM_LENGTH) {
        len = cnt_length(t);
        if (len > 0) {
            res /= (float)len;
        }
    }

    /* RANK_NORM_EXTDIST not applicable */
    if (((unsigned int4)method & RANK_NORM_UNIQ) && t->size > 0) {
        res /= (float)(t->size);
    }

    if (((unsigned int4)method & RANK_NORM_LOGUNIQ) && t->size > 0) {
        res /= log((double)(t->size + 1)) / log(2.0);
    }

    if ((unsigned int4)method & RANK_NORM_RDIVRPLUS1) {
        res /= (res + 1);
    }

    return res;
}

static float* get_weights(ArrayType* win)
{
    static float ws[lengthof(weights)];
    int i;
    float4* arrdata = NULL;

    if (win == NULL) {
        return weights;
    }

    if (ARR_NDIM(win) != 1) {
        ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), errmsg("array of weight must be one-dimensional")));
    }

    if ((unsigned int)(ArrayGetNItems(ARR_NDIM(win), ARR_DIMS(win))) < lengthof(weights)) {
        ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), errmsg("array of weight is too short")));
    }

    if (array_contains_nulls(win)) {
        ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("array of weight must not contain nulls")));
    }

    arrdata = (float4*)ARR_DATA_PTR(win);
    for (i = 0; (unsigned int)(i) < lengthof(weights); i++) {
        ws[i] = (arrdata[i] >= 0) ? arrdata[i] : weights[i];
        if (ws[i] > 1.0) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("weight out of range")));
        }
    }

    return ws;
}

Datum ts_rank_wttf(PG_FUNCTION_ARGS)
{
    ts_check_feature_disable();
    ArrayType* win = (ArrayType*)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
    TSVector txt = PG_GETARG_TSVECTOR(1);
    TSQuery query = PG_GETARG_TSQUERY(2);
    int method = PG_GETARG_INT32(3);
    float res = calc_rank(get_weights(win), txt, query, method);

    PG_FREE_IF_COPY(win, 0);
    PG_FREE_IF_COPY(txt, 1);
    PG_FREE_IF_COPY(query, 2);
    PG_RETURN_FLOAT4(res);
}

Datum ts_rank_wtt(PG_FUNCTION_ARGS)
{
    ts_check_feature_disable();
    ArrayType* win = (ArrayType*)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
    TSVector txt = PG_GETARG_TSVECTOR(1);
    TSQuery query = PG_GETARG_TSQUERY(2);
    float res = calc_rank(get_weights(win), txt, query, DEF_NORM_METHOD);

    PG_FREE_IF_COPY(win, 0);
    PG_FREE_IF_COPY(txt, 1);
    PG_FREE_IF_COPY(query, 2);
    PG_RETURN_FLOAT4(res);
}

Datum ts_rank_ttf(PG_FUNCTION_ARGS)
{
    ts_check_feature_disable();
    TSVector txt = PG_GETARG_TSVECTOR(0);
    TSQuery query = PG_GETARG_TSQUERY(1);
    int method = PG_GETARG_INT32(2);
    float res = calc_rank(get_weights(NULL), txt, query, method);

    PG_FREE_IF_COPY(txt, 0);
    PG_FREE_IF_COPY(query, 1);
    PG_RETURN_FLOAT4(res);
}

Datum ts_rank_tt(PG_FUNCTION_ARGS)
{
    ts_check_feature_disable();
    TSVector txt = PG_GETARG_TSVECTOR(0);
    TSQuery query = PG_GETARG_TSQUERY(1);
    float res = calc_rank(get_weights(NULL), txt, query, DEF_NORM_METHOD);

    PG_FREE_IF_COPY(txt, 0);
    PG_FREE_IF_COPY(query, 1);
    PG_RETURN_FLOAT4(res);
}

typedef struct {
    QueryItem** item;
    int16 nitem;
    uint8 wclass;
    int32 pos;
} DocRepresentation;

static int compareDocR(const void* va, const void* vb)
{
    const DocRepresentation* a = (const DocRepresentation*)va;
    const DocRepresentation* b = (const DocRepresentation*)vb;

    if (a->pos == b->pos) {
        return 0;
    }
    return (a->pos > b->pos) ? 1 : -1;
}

typedef struct {
    TSQuery query;
    bool* operandexist;
} QueryRepresentation;

#define QR_GET_OPERAND_EXISTS(q, v) ((q)->operandexist[((QueryItem*)(v)) - GETQUERY((q)->query)])
#define QR_SET_OPERAND_EXISTS(q, v) QR_GET_OPERAND_EXISTS(q, v) = true

static bool check_condition_query_operand(const void* checkval, QueryOperand* val)
{
    QueryRepresentation* qr = (QueryRepresentation*)checkval;
    return QR_GET_OPERAND_EXISTS(qr, val);
}

typedef struct {
    int pos;
    int p;
    int q;
    DocRepresentation* begin;
    DocRepresentation* end;
} Extention;

static bool cover(DocRepresentation* doc, int len, QueryRepresentation* qr, Extention* ext)
{
    DocRepresentation* ptr = NULL;
    int lastpos = ext->pos;
    int i;
    bool found = false;
    errno_t rc = EOK;

    /*
     * since this function recurses, it could be driven to stack overflow.
     * (though any decent compiler will optimize away the tail-recursion.
     */
    check_stack_depth();

    rc = memset_s(qr->operandexist, sizeof(bool) * qr->query->size, 0, sizeof(bool) * qr->query->size);
    securec_check(rc, "\0", "\0");
    ext->p = 0x7fffffff;
    ext->q = 0;
    ptr = doc + ext->pos;
    /* find upper bound of cover from current position, move up */
    while (ptr - doc < len) {
        for (i = 0; i < ptr->nitem; i++) {
            if (ptr->item[i]->type == QI_VAL) {
                QR_SET_OPERAND_EXISTS(qr, ptr->item[i]);
            }
        }
        if (TS_execute(GETQUERY(qr->query), (void*)qr, false, check_condition_query_operand)) {
            if (ptr->pos > ext->q) {
                ext->q = ptr->pos;
                ext->end = ptr;
                lastpos = ptr - doc;
                found = true;
            }
            break;
        }
        ptr++;
    }

    if (!found) {
        return false;
    }

    rc = memset_s(qr->operandexist, sizeof(bool) * qr->query->size, 0, sizeof(bool) * qr->query->size);
    securec_check(rc, "\0", "\0");
    ptr = doc + lastpos;
    /* find lower bound of cover from found upper bound, move down */
    while (ptr >= doc + ext->pos) {
        for (i = 0; i < ptr->nitem; i++) {
            if (ptr->item[i]->type == QI_VAL) {
                QR_SET_OPERAND_EXISTS(qr, ptr->item[i]);
            }
        }
        if (TS_execute(GETQUERY(qr->query), (void*)qr, true, check_condition_query_operand)) {
            if (ptr->pos < ext->p) {
                ext->begin = ptr;
                ext->p = ptr->pos;
            }
            break;
        }
        ptr--;
    }

    if (ext->p <= ext->q) {
        /*
         * set position for next try to next lexeme after beginning of found
         * cover
         */
        ext->pos = (ptr - doc) + 1;
        return true;
    }

    ext->pos++;
    return cover(doc, len, qr, ext);
}

static DocRepresentation* get_docrep(TSVector txt, QueryRepresentation* qr, int* doclen)
{
    QueryItem* item = GETQUERY(qr->query);
    WordEntry* entry = NULL;
    WordEntry* firstentry = NULL;
    WordEntryPos* post = NULL;
    int4 dimt, j, i, nitem;
    int len = qr->query->size * 4;
    int cur = 0;
    DocRepresentation* doc = NULL;
    char* operand = NULL;

    /* A dummy WordEntryPos array to use when haspos is false */
    WordEntryPosVector* pos_null_ptr = (WordEntryPosVector*)palloc(sizeof(WordEntryPosVector) + sizeof(WordEntryPos));
    pos_null_ptr->npos = 1;
    pos_null_ptr->pos[0] = 0;
    WordEntryPosVector POSNULL = *pos_null_ptr;

    doc = (DocRepresentation*)palloc(sizeof(DocRepresentation) * len);
    operand = GETOPERAND(qr->query);
    for (i = 0; i < qr->query->size; i++) {
        QueryOperand* curoperand = NULL;
        if (item[i].type != QI_VAL) {
            continue;
        }

        curoperand = &item[i].qoperand;
        if (QR_GET_OPERAND_EXISTS(qr, &item[i])) {
            continue;
        }

        firstentry = entry = find_wordentry(txt, qr->query, curoperand, &nitem);
        if (entry == NULL) {
            continue;
        }

        while (entry - firstentry < nitem) {
            if (entry->haspos) {
                dimt = POSDATALEN(txt, entry);
                post = POSDATAPTR(txt, entry);
            } else {
                dimt = POSNULL.npos;
                post = POSNULL.pos;
            }

            while (cur + dimt >= len) {
                len *= 2;
                doc = (DocRepresentation*)repalloc(doc, sizeof(DocRepresentation) * len);
            }

            for (j = 0; j < dimt; j++) {
                if (j == 0) {
                    int k;
                    doc[cur].nitem = 0;
                    doc[cur].item = (QueryItem**)palloc(sizeof(QueryItem*) * qr->query->size);
                    for (k = 0; k < qr->query->size; k++) {
                        QueryOperand* kptr = &item[k].qoperand;
                        QueryOperand* iptr = &item[i].qoperand;

                        if (k == i || (item[k].type == QI_VAL && compare_query_operand(&kptr, &iptr, operand) == 0)) {
                            /*
                             * if k == i, we've already checked above that
                             * it's type == Q_VAL
                             */
                            doc[cur].item[doc[cur].nitem] = item + k;
                            doc[cur].nitem++;
                            QR_SET_OPERAND_EXISTS(qr, item + k);
                        }
                    }
                } else {
                    doc[cur].nitem = doc[cur - 1].nitem;
                    doc[cur].item = doc[cur - 1].item;
                }
                doc[cur].pos = WEP_GETPOS(post[j]);
                doc[cur].wclass = WEP_GETWEIGHT(post[j]);
                cur++;
            }
            entry++;
        }
    }

    *doclen = cur;
    if (cur > 0) {
        qsort((void*)doc, cur, sizeof(DocRepresentation), compareDocR);
        return doc;
    }

    pfree_ext(pos_null_ptr);
    pfree_ext(doc);
    return NULL;
}

static float4 calc_rank_cd(float4* arrdata, TSVector txt, TSQuery query, int method)
{
    DocRepresentation* doc = NULL;
    int len;
    int i;
    int doclen = 0;
    Extention ext;
    double wdoc = 0.0;
    double invws[lengthof(weights)];
    double sum_dist = 0.0;
    double prev_ext_pos = 0.0;
    double cur_ext_pos = 0.0;
    int n_ext_ent = 0;
    QueryRepresentation qr;
    errno_t rc = 0;

    for (i = 0; (unsigned int)(i) < lengthof(weights); i++) {
        invws[i] = ((double)((arrdata[i] >= 0) ? arrdata[i] : weights[i]));
        if (invws[i] > 1.0) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("weight out of range")));
        }
        invws[i] = 1.0 / invws[i];
    }

    qr.query = query;
    qr.operandexist = (bool*)palloc0(sizeof(bool) * query->size);
    doc = get_docrep(txt, &qr, &doclen);
    if (doc == NULL) {
        pfree_ext(qr.operandexist);
        return 0.0;
    }

    rc = memset_s(&ext, sizeof(Extention), 0, sizeof(Extention));
    securec_check(rc, "\0", "\0");
    while (cover(doc, doclen, &qr, &ext)) {
        double cpos = 0.0;
        double inv_sum = 0.0;
        int n_noise;
        DocRepresentation* ptr = ext.begin;
        while (ptr <= ext.end) {
            inv_sum += invws[ptr->wclass];
            ptr++;
        }

        cpos = ((double)(ext.end - ext.begin + 1)) / inv_sum;
        /*
         * if doc are big enough then ext.q may be equal to ext.p due to limit
         * of posional information. In this case we approximate number of
         * noise word as half cover's length
         */
        n_noise = (ext.q - ext.p) - (ext.end - ext.begin);
        if (n_noise < 0) {
            n_noise = (ext.end - ext.begin) / 2;
        }
        wdoc += cpos / ((double)(1 + n_noise));

        cur_ext_pos = ((double)(ext.q + ext.p)) / 2.0;
        if (n_ext_ent > 0 && cur_ext_pos > prev_ext_pos) {
            /* prevent devision by
            * zero in a case of
            * multiple lexize */
            sum_dist += 1.0 / (cur_ext_pos - prev_ext_pos);
        }

        prev_ext_pos = cur_ext_pos;
        n_ext_ent++;
    }

    if ((method & RANK_NORM_LOGLENGTH) && txt->size > 0) {
        wdoc /= log((double)(cnt_length(txt) + 1));
    }

    if (method & RANK_NORM_LENGTH) {
        len = cnt_length(txt);
        if (len > 0) {
            wdoc /= (double)len;
        }
    }

    if ((method & RANK_NORM_EXTDIST) && n_ext_ent > 0 && sum_dist > 0) {
        wdoc /= ((double)n_ext_ent) / sum_dist;
    }

    if ((method & RANK_NORM_UNIQ) && txt->size > 0) {
        wdoc /= (double)(txt->size);
    }

    if ((method & RANK_NORM_LOGUNIQ) && txt->size > 0) {
        wdoc /= log((double)(txt->size + 1)) / log(2.0);
    }

    if (method & RANK_NORM_RDIVRPLUS1) {
        wdoc /= (wdoc + 1);
    }

    pfree_ext(doc);
    pfree_ext(qr.operandexist);
    return (float4)wdoc;
}

Datum ts_rankcd_wttf(PG_FUNCTION_ARGS)
{
    ts_check_feature_disable();
    ArrayType* win = (ArrayType*)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
    TSVector txt = PG_GETARG_TSVECTOR(1);
    TSQuery query = PG_GETARG_TSQUERY(2);
    int method = PG_GETARG_INT32(3);
    float res;

    res = calc_rank_cd(get_weights(win), txt, query, method);
    PG_FREE_IF_COPY(win, 0);
    PG_FREE_IF_COPY(txt, 1);
    PG_FREE_IF_COPY(query, 2);
    PG_RETURN_FLOAT4(res);
}

Datum ts_rankcd_wtt(PG_FUNCTION_ARGS)
{
    ts_check_feature_disable();
    ArrayType* win = (ArrayType*)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
    TSVector txt = PG_GETARG_TSVECTOR(1);
    TSQuery query = PG_GETARG_TSQUERY(2);
    float res;

    res = calc_rank_cd(get_weights(win), txt, query, DEF_NORM_METHOD);

    PG_FREE_IF_COPY(win, 0);
    PG_FREE_IF_COPY(txt, 1);
    PG_FREE_IF_COPY(query, 2);
    PG_RETURN_FLOAT4(res);
}

Datum ts_rankcd_ttf(PG_FUNCTION_ARGS)
{
    ts_check_feature_disable();
    TSVector txt = PG_GETARG_TSVECTOR(0);
    TSQuery query = PG_GETARG_TSQUERY(1);
    int method = PG_GETARG_INT32(2);
    float res;

    res = calc_rank_cd(get_weights(NULL), txt, query, method);
    PG_FREE_IF_COPY(txt, 0);
    PG_FREE_IF_COPY(query, 1);
    PG_RETURN_FLOAT4(res);
}

Datum ts_rankcd_tt(PG_FUNCTION_ARGS)
{
    ts_check_feature_disable();
    TSVector txt = PG_GETARG_TSVECTOR(0);
    TSQuery query = PG_GETARG_TSQUERY(1);
    float res;

    res = calc_rank_cd(get_weights(NULL), txt, query, DEF_NORM_METHOD);
    PG_FREE_IF_COPY(txt, 0);
    PG_FREE_IF_COPY(query, 1);
    PG_RETURN_FLOAT4(res);
}