/* This file is part of Pazpar2.
   Copyright (C) 2006-2013 Index Data

Pazpar2 is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.

Pazpar2 is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#include <assert.h>

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <yaz/yaz-util.h>

#include "ppmutex.h"
#include "session.h"
#include "reclists.h"
#include "jenkins_hash.h"
#include "parameters.h"

struct reclist
{
    struct reclist_bucket **hashtable;
    unsigned hash_size;

    int num_records;
    int all_ingested_num;
    struct reclist_bucket *sorted_list;
    struct reclist_bucket *sorted_ptr;
    struct reclist_bucket **last;
    struct record *all_records; // linked list of all records ingested in a session, in ingestion order (last ingested is the head)
    NMEM nmem;
    YAZ_MUTEX mutex;
};

struct reclist_bucket
{
    struct record_cluster *record;
    struct reclist_bucket *hash_next;
    struct reclist_bucket *sorted_next;
    struct reclist_sortparms *sort_parms;
};

struct reclist_sortparms *reclist_parse_sortparms(NMEM nmem, const char *parms,
                                                  struct conf_service *service)
{
    struct reclist_sortparms *res = 0;
    struct reclist_sortparms **rp = &res;

    if (strlen(parms) > 256)
        return 0;
    while (*parms)
    {
        char parm[256];
        char *pp;
        const char *cpp;
        int increasing = 0;
        int i;
        int offset = 0;
        enum conf_sortkey_type type = Metadata_sortkey_string;
        struct reclist_sortparms *new;

        if (!(cpp = strchr(parms, ',')))
            cpp = parms + strlen(parms);
        strncpy(parm, parms, cpp - parms);
        parm[cpp-parms] = '\0';

        if ((pp = strchr(parm, ':')))
        {
            if (pp[1] == '1')
                increasing = 1;
            else if (pp[1] == '0')
                increasing = 0;
            else
            {
                yaz_log(YLOG_FATAL, "Bad sortkey modifier: %s", parm);
                return 0;
            }

            if (pp[2])
            {
                if (pp[2] == 'p')
                    type = Metadata_sortkey_position;
                else
                    yaz_log(YLOG_FATAL, "Bad sortkey modifier: %s", parm);
            }
            *pp = '\0';
        }
        if (type != Metadata_sortkey_position)
        {
            if (!strcmp(parm, "relevance"))
            {
                type = Metadata_sortkey_relevance;
            }
            else if (!strcmp(parm, "position"))
            {
                type = Metadata_sortkey_position;
            }
            else
            {
                for (i = 0; i < service->num_sortkeys; i++)
                {
                    struct conf_sortkey *sk = &service->sortkeys[i];
                    if (!strcmp(sk->name, parm))
                    {
                        type = sk->type;
                        if (type == Metadata_sortkey_skiparticle)
                            type = Metadata_sortkey_string;
                        break;
                    }
                }
                if (i >= service->num_sortkeys)
                {
                    yaz_log(YLOG_FATAL, "Sortkey not defined in service: %s",
                            parm);
                    return 0;
                }
                offset = i;
            }
        }
        new = *rp = nmem_malloc(nmem, sizeof(struct reclist_sortparms));
        new->next = 0;
        new->offset = offset;
        new->type = type;
        new->increasing = increasing;
        new->name = nmem_strdup(nmem, parm);
        rp = &new->next;
        if (*(parms = cpp))
            parms++;
    }
    return res;
}

static int reclist_cmp(const void *p1, const void *p2)
{
    struct reclist_sortparms *sortparms =
        (*(struct reclist_bucket **) p1)->sort_parms;
    struct record_cluster *r1 = (*(struct reclist_bucket**) p1)->record;
    struct record_cluster *r2 = (*(struct reclist_bucket**) p2)->record;
    struct reclist_sortparms *s;
    int res = 0;

    for (s = sortparms; s && res == 0; s = s->next)
    {
        union data_types *ut1 = r1->sortkeys[s->offset];
        union data_types *ut2 = r2->sortkeys[s->offset];
        const char *s1, *s2;
        switch (s->type)
        {
        case Metadata_sortkey_relevance:
            res = r2->relevance_score - r1->relevance_score;
            break;
        case Metadata_sortkey_string:
            s1 = ut1 ? ut1->text.sort : "";
            s2 = ut2 ? ut2->text.sort : "";
            res = strcmp(s2, s1);
            if (res)
            {
                if (s->increasing)
                    res *= -1;
            }
            break;
        case Metadata_sortkey_numeric:
            if (ut1 && ut2)
            {
                if (s->increasing)
                    res = ut1->number.min  - ut2->number.min;
                else
                    res = ut2->number.max  - ut1->number.max;
            }
            else if (ut1 && !ut2)
                res = -1;
            else if (!ut1 && ut2)
                res = 1;
            else
                res = 0;
            break;
        case Metadata_sortkey_position:
            if (r1->records && r2->records)
            {
                int pos1 = 0, pos2 = 0;
                struct record *rec;
                for (rec = r1->records; rec; rec = rec->next)
                    if (pos1 == 0 || rec->position < pos1)
                        pos1 = rec->position;
                for (rec = r2->records; rec; rec = rec->next)
                    if (pos2 == 0 || rec->position < pos2)
                        pos2 = rec->position;
                res = pos1 - pos2;
            }
            break;
        default:
            yaz_log(YLOG_WARN, "Bad sort type: %d", s->type);
            res = 0;
            break;
        }
    }
    if (res == 0)
        res = strcmp(r1->recid, r2->recid);
    return res;
}

void reclist_limit(struct reclist *l, struct session *se)
{
    unsigned i;
    int num = 0;
    struct reclist_bucket **pp = &l->sorted_list;

    reclist_enter(l);
    for (i = 0; i < l->hash_size; i++)
    {
        struct reclist_bucket *p;
        for (p = l->hashtable[i]; p; p = p->hash_next)
        {
            if (session_check_cluster_limit(se, p->record))
            {
                *pp = p;
                pp = &p->sorted_next;
                num++;
            }
            else
            {
                yaz_log(YLOG_LOG, "session_check_cluster returned false");
            }
        }
    }
    *pp = 0;
    l->num_records = num;
    reclist_leave(l);
}

void reclist_sort(struct reclist *l, struct reclist_sortparms *parms)
{
    struct reclist_bucket **flatlist = xmalloc(sizeof(*flatlist) * l->num_records);
    struct reclist_bucket *ptr;
    struct reclist_bucket **prev;
    int i = 0;

    reclist_enter(l);

    ptr = l->sorted_list;
    prev = &l->sorted_list;
    while (ptr)
    {
        ptr->sort_parms = parms;
        flatlist[i] = ptr;
        ptr = ptr->sorted_next;
        i++;
    }
    assert(i == l->num_records);

    qsort(flatlist, l->num_records, sizeof(*flatlist), reclist_cmp);
    for (i = 0; i < l->num_records; i++)
    {
        *prev = flatlist[i];
        prev = &flatlist[i]->sorted_next;
    }
    *prev = 0;

    xfree(flatlist);

    reclist_leave(l);
}

struct record_cluster *reclist_read_record(struct reclist *l)
{
    if (l && l->sorted_ptr)
    {
        struct record_cluster *t = l->sorted_ptr->record;
        l->sorted_ptr = l->sorted_ptr->sorted_next;
        return t;
    }
    else
        return 0;
}

void reclist_enter(struct reclist *l)
{
    yaz_mutex_enter(l->mutex);
    if (l)
        l->sorted_ptr = l->sorted_list;
}


void reclist_leave(struct reclist *l)
{
    yaz_mutex_leave(l->mutex);
    if (l)
        l->sorted_ptr = l->sorted_list;
}


struct reclist *reclist_create(NMEM nmem)
{
    struct reclist *res = nmem_malloc(nmem, sizeof(struct reclist));
    res->hash_size = 399;
    res->hashtable
        = nmem_malloc(nmem, res->hash_size * sizeof(struct reclist_bucket*));
    memset(res->hashtable, 0, res->hash_size * sizeof(struct reclist_bucket*));
    res->nmem = nmem;

    res->sorted_ptr = 0;
    res->sorted_list = 0;
    res->last = &res->sorted_list;
    res->all_records = 0;
    res->all_ingested_num = 0;

    res->num_records = 0;
    res->mutex = 0;
    pazpar2_mutex_create(&res->mutex, "reclist");
    return res;
}

void reclist_destroy(struct reclist *l)
{
    if (l)
    {
        unsigned i;
        for (i = 0; i < l->hash_size; i++)
        {
            struct reclist_bucket *p;
            for (p = l->hashtable[i]; p; p = p->hash_next)
            {
                wrbuf_destroy(p->record->relevance_explain1);
                wrbuf_destroy(p->record->relevance_explain2);
            }
        }
        yaz_mutex_destroy(&l->mutex);
    }
}

int reclist_get_num_records(struct reclist *l)
{
    if (l)
        return l->num_records;
    return 0;
}

int reclist_get_num_ingested(struct reclist *l)
{
    return l ? l->all_ingested_num : 0;
}


struct record *reclist_get_ingested(struct reclist *l)
{
    return l ? l->all_records : 0;
}


/**
 * Append a new record at the *start* of the `all_records` list.
 */
int reclist_ingest(struct reclist *l, struct record *record)
{
    assert(l);
    assert(record);

    yaz_mutex_enter(l->mutex);

    struct record *ingested_rec = record_copy(l->nmem, record);
    ingested_rec->next = l->all_records;
    l->all_records = ingested_rec;
    l->all_ingested_num++;

    yaz_mutex_leave(l->mutex);
    return 0;
}


// Insert a record. Return record cluster (newly formed or pre-existing)
struct record_cluster *reclist_insert(struct reclist *l,
                                      struct conf_service *service,
                                      struct record *record,
                                      const char *merge_key, int *total)
{
    unsigned int bucket;
    struct reclist_bucket **p;
    struct record_cluster *cluster = 0;

    assert(service);
    assert(l);
    assert(record);
    assert(merge_key);
    assert(total);

    bucket = jenkins_hash((unsigned char*) merge_key) % l->hash_size;

    yaz_mutex_enter(l->mutex);
    for (p = &l->hashtable[bucket]; *p; p = &(*p)->hash_next)
    {
        // We found a matching record. Merge them
        if (!strcmp(merge_key, (*p)->record->merge_key))
        {
            struct record_cluster *existing = (*p)->record;
            struct record *re = existing->records;

            for (; re; re = re->next)
            {
                if (re->client == record->client &&
                    record_compare(record, re, service))
                {
                    yaz_mutex_leave(l->mutex);
                    return 0;
                }
            }
            record->next = existing->records;
            existing->records = record;
            cluster = existing;
            break;
        }
    }
    if (!cluster)
    {
        struct reclist_bucket *new =
            nmem_malloc(l->nmem, sizeof(*new));

        cluster = nmem_malloc(l->nmem, sizeof(*cluster));

        record->next = 0;
        new->record = cluster;
        new->hash_next = 0;
        cluster->records = record;
        cluster->merge_key = nmem_strdup(l->nmem, merge_key);
        cluster->relevance_score = 0;
        cluster->term_frequency_vec = 0;
        cluster->recid = nmem_strdup(l->nmem, merge_key);
        (*total)++;
        cluster->metadata =
            nmem_malloc(l->nmem,
                        sizeof(struct record_metadata*) * service->num_metadata);
        memset(cluster->metadata, 0,
               sizeof(struct record_metadata*) * service->num_metadata);
        cluster->sortkeys =
            nmem_malloc(l->nmem, sizeof(struct record_metadata*) * service->num_sortkeys);
        memset(cluster->sortkeys, 0,
               sizeof(union data_types*) * service->num_sortkeys);

        cluster->relevance_explain1 = wrbuf_alloc();
        cluster->relevance_explain2 = wrbuf_alloc();
        /* attach to hash list */
        *p = new;
        l->num_records++;
    }

    yaz_mutex_leave(l->mutex);
    return cluster;
}

int reclist_sortparms_cmp(struct reclist_sortparms *sort1, struct reclist_sortparms *sort2)
{
    int rc;
    if (sort1 == sort2)
        return 0;
    if (sort1 == 0 || sort2 == 0)
        return 1;
    rc = strcmp(sort1->name, sort2->name) || sort1->increasing != sort2->increasing || sort1->type != sort2->type;
    return rc;
}
/*
 * Local variables:
 * c-basic-offset: 4
 * c-file-style: "Stroustrup"
 * indent-tabs-mode: nil
 * End:
 * vim: shiftwidth=4 tabstop=8 expandtab
 */

