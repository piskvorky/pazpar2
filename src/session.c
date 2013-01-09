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

/** \file session.c
    \brief high-level logic; mostly user sessions and settings
*/

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef WIN32
#include <windows.h>
#endif
#include <signal.h>
#include <ctype.h>
#include <assert.h>
#include <math.h>

#include <yaz/marcdisp.h>
#include <yaz/comstack.h>
#include <yaz/tcpip.h>
#include <yaz/proto.h>
#include <yaz/readconf.h>
#include <yaz/pquery.h>
#include <yaz/otherinfo.h>
#include <yaz/yaz-util.h>
#include <yaz/nmem.h>
#include <yaz/query-charset.h>
#include <yaz/querytowrbuf.h>
#include <yaz/oid_db.h>
#include <yaz/snprintf.h>

#define USE_TIMING 0
#if USE_TIMING
#include <yaz/timing.h>
#endif

#include "ppmutex.h"
#include "parameters.h"
#include "session.h"
#include "eventl.h"
#include "http.h"
#include "termlists.h"
#include "reclists.h"
#include "relevance.h"
#include "database.h"
#include "client.h"
#include "settings.h"
#include "normalize7bit.h"

#include <libxml/tree.h>

#define MAX_CHUNK 15

#define MAX(a,b) ((a)>(b)?(a):(b))

// Note: Some things in this structure will eventually move to configuration
struct parameters global_parameters =
{
    0,   // dump_records
    0,   // debug_mode
    0,   // predictable sessions
};

struct client_list {
    struct client *client;
    struct client_list *next;
};

/* session counting (1) , disable client counting (0) */
static YAZ_MUTEX g_session_mutex = 0;
static int no_sessions = 0;
static int no_session_total = 0;

static int session_use(int delta)
{
    int sessions;
    if (!g_session_mutex)
        yaz_mutex_create(&g_session_mutex);
    yaz_mutex_enter(g_session_mutex);
    no_sessions += delta;
    if (delta > 0)
        no_session_total += delta;
    sessions = no_sessions;
    yaz_mutex_leave(g_session_mutex);
    yaz_log(YLOG_DEBUG, "%s sessions=%d", delta == 0 ? "" : (delta > 0 ? "INC" : "DEC"), no_sessions);
    return sessions;
}

int sessions_count(void)
{
    return session_use(0);
}

int session_count_total(void)
{
    int total = 0;
    if (!g_session_mutex)
        return 0;
    yaz_mutex_enter(g_session_mutex);
    total = no_session_total;
    yaz_mutex_leave(g_session_mutex);
    return total;
}

static void log_xml_doc(xmlDoc *doc)
{
    FILE *lf = yaz_log_file();
    xmlChar *result = 0;
    int len = 0;
#if LIBXML_VERSION >= 20600
    xmlDocDumpFormatMemory(doc, &result, &len, 1);
#else
    xmlDocDumpMemory(doc, &result, &len);
#endif
    if (lf && len)
    {
        (void) fwrite(result, 1, len, lf);
        fprintf(lf, "\n");
    }
    xmlFree(result);
}

static void session_enter(struct session *s, const char *caller)
{
    if (caller)
        session_log(s, YLOG_DEBUG, "Session lock by %s", caller);
    yaz_mutex_enter(s->session_mutex);
}

static void session_leave(struct session *s, const char *caller)
{
    yaz_mutex_leave(s->session_mutex);
    if (caller)
        session_log(s, YLOG_DEBUG, "Session unlock by %s", caller);
}

static void session_normalize_facet(struct session *s, const char *type,
                                    const char *value,
                                    WRBUF display_wrbuf,
                                    WRBUF facet_wrbuf)
{
    struct conf_service *service = s->service;
    pp2_charset_token_t prt;
    const char *facet_component;
    int i;
    const char *icu_chain_id = 0;

    for (i = 0; i < service->num_metadata; i++)
        if (!strcmp((service->metadata + i)->name, type))
            icu_chain_id = (service->metadata + i)->facetrule;
    if (!icu_chain_id)
        icu_chain_id = "facet";
    prt = pp2_charset_token_create(service->charsets, icu_chain_id);
    if (!prt)
    {
        yaz_log(YLOG_FATAL, "Unknown ICU chain '%s' for facet of type '%s'",
                icu_chain_id, type);
        wrbuf_destroy(facet_wrbuf);
        wrbuf_destroy(display_wrbuf);
        return;
    }
    pp2_charset_token_first(prt, value, 0);
    while ((facet_component = pp2_charset_token_next(prt)))
    {
        const char *display_component;
        if (*facet_component)
        {
            if (wrbuf_len(facet_wrbuf))
                wrbuf_puts(facet_wrbuf, " ");
            wrbuf_puts(facet_wrbuf, facet_component);
        }
        display_component = pp2_get_display(prt);
        if (display_component)
        {
            if (wrbuf_len(display_wrbuf))
                wrbuf_puts(display_wrbuf, " ");
            wrbuf_puts(display_wrbuf, display_component);
        }
    }
    pp2_charset_token_destroy(prt);
}

void add_facet(struct session *s, const char *type, const char *value, int count)
{
    WRBUF facet_wrbuf = wrbuf_alloc();
    WRBUF display_wrbuf = wrbuf_alloc();

    session_normalize_facet(s, type, value, display_wrbuf, facet_wrbuf);

    if (wrbuf_len(facet_wrbuf))
    {
        int i;
        for (i = 0; i < s->num_termlists; i++)
            if (!strcmp(s->termlists[i].name, type))
                break;
        if (i == s->num_termlists)
        {
            if (i == SESSION_MAX_TERMLISTS)
            {
                session_log(s, YLOG_FATAL, "Too many termlists");
                wrbuf_destroy(facet_wrbuf);
                wrbuf_destroy(display_wrbuf);
                return;
            }

            s->termlists[i].name = nmem_strdup(s->nmem, type);
            s->termlists[i].termlist = termlist_create(s->nmem);
            s->num_termlists = i + 1;
        }

#if 0
        session_log(s, YLOG_LOG, "Facets for %s: %s norm:%s (%d)", type, value, wrbuf_cstr(facet_wrbuf), count);
#endif
        termlist_insert(s->termlists[i].termlist, wrbuf_cstr(display_wrbuf),
                        wrbuf_cstr(facet_wrbuf), count);
    }
    wrbuf_destroy(facet_wrbuf);
    wrbuf_destroy(display_wrbuf);
}

static xmlDoc *record_to_xml(struct session *se,
                             struct session_database *sdb, const char *rec)
{
    struct database *db = sdb->database;
    xmlDoc *rdoc = 0;

    rdoc = xmlParseMemory(rec, strlen(rec));

    if (!rdoc)
    {
        session_log(se, YLOG_WARN, "Non-wellformed XML");
        return 0;
    }

    if (global_parameters.dump_records)
    {
        session_log(se, YLOG_LOG, "Un-normalized record from %s", db->id);
        log_xml_doc(rdoc);
    }

    return rdoc;
}

#define MAX_XSLT_ARGS 16

// Add static values from session database settings if applicable
static void insert_settings_parameters(struct session_database *sdb,
                                       struct conf_service *service,
                                       char **parms,
                                       NMEM nmem)
{
    int i;
    int nparms = 0;
    int offset = 0;

    for (i = 0; i < service->num_metadata; i++)
    {
        struct conf_metadata *md = &service->metadata[i];
        int setting;

        if (md->setting == Metadata_setting_parameter &&
            (setting = settings_lookup_offset(service, md->name)) >= 0)
        {
            const char *val = session_setting_oneval(sdb, setting);
            if (val && nparms < MAX_XSLT_ARGS)
            {
                char *buf;
                int len = strlen(val);
                buf = nmem_malloc(nmem, len + 3);
                buf[0] = '\'';
                strcpy(buf + 1, val);
                buf[len+1] = '\'';
                buf[len+2] = '\0';
                parms[offset++] = md->name;
                parms[offset++] = buf;
                nparms++;
            }
        }
    }
    parms[offset] = 0;
}

// Add static values from session database settings if applicable
static void insert_settings_values(struct session_database *sdb, xmlDoc *doc,
    struct conf_service *service)
{
    int i;

    for (i = 0; i < service->num_metadata; i++)
    {
        struct conf_metadata *md = &service->metadata[i];
        int offset;

        if (md->setting == Metadata_setting_postproc &&
            (offset = settings_lookup_offset(service, md->name)) >= 0)
        {
            const char *val = session_setting_oneval(sdb, offset);
            if (val)
            {
                xmlNode *r = xmlDocGetRootElement(doc);
                xmlNode *n = xmlNewTextChild(r, 0, (xmlChar *) "metadata",
                                             (xmlChar *) val);
                xmlSetProp(n, (xmlChar *) "type", (xmlChar *) md->name);
            }
        }
    }
}

static xmlDoc *normalize_record(struct session *se,
                                struct session_database *sdb,
                                struct conf_service *service,
                                const char *rec, NMEM nmem)
{
    xmlDoc *rdoc = record_to_xml(se, sdb, rec);

    if (rdoc)
    {
        char *parms[MAX_XSLT_ARGS*2+1];

        insert_settings_parameters(sdb, service, parms, nmem);

        if (normalize_record_transform(sdb->map, &rdoc, (const char **)parms))
        {
            session_log(se, YLOG_WARN, "Normalize failed");
        }
        else
        {
            insert_settings_values(sdb, rdoc, service);

            if (global_parameters.dump_records)
            {
                session_log(se, YLOG_LOG, "Normalized record from %s",
                            sdb->database->id);
                log_xml_doc(rdoc);
            }
        }
    }
    return rdoc;
}

void session_settings_dump(struct session *se,
                           struct session_database *db,
                           WRBUF w)
{
    if (db->settings)
    {
        int i, num = db->num_settings;
        for (i = 0; i < num; i++)
        {
            struct setting *s = db->settings[i];
            for (;s ; s = s->next)
            {
                wrbuf_puts(w, "<set name=\"");
                wrbuf_xmlputs(w, s->name);
                wrbuf_puts(w, "\" value=\"");
                wrbuf_xmlputs(w, s->value);
                wrbuf_puts(w, "\"/>");
            }
            if (db->settings[i])
                wrbuf_puts(w, "\n");
        }
    }
}

// Retrieve first defined value for 'name' for given database.
// Will be extended to take into account user associated with session
const char *session_setting_oneval(struct session_database *db, int offset)
{
    if (offset >= db->num_settings || !db->settings[offset])
        return "";
    return db->settings[offset]->value;
}

// Prepare XSLT stylesheets for record normalization
// Structures are allocated on the session_wide nmem to avoid having
// to recompute this for every search. This would lead
// to leaking if a single session was to repeatedly change the PZ_XSLT
// setting. However, this is not a realistic use scenario.
static int prepare_map(struct session *se, struct session_database *sdb)
{
    if (sdb->settings && !sdb->map)
    {
        const char *s;

        if (sdb->settings[PZ_XSLT] &&
            (s = session_setting_oneval(sdb, PZ_XSLT)))
        {
            char auto_stylesheet[256];

            if (!strcmp(s, "auto"))
            {
                const char *request_syntax = session_setting_oneval(
                    sdb, PZ_REQUESTSYNTAX);
                if (request_syntax)
                {
                    char *cp;
                    yaz_snprintf(auto_stylesheet, sizeof(auto_stylesheet),
                                 "%s.xsl", request_syntax);
                    for (cp = auto_stylesheet; *cp; cp++)
                    {
                        /* deliberately only consider ASCII */
                        if (*cp > 32 && *cp < 127)
                            *cp = tolower(*cp);
                    }
                    s = auto_stylesheet;
                }
                else
                {
                    session_log(se, YLOG_WARN,
                                "No pz:requestsyntax for auto stylesheet");
                }
            }
            sdb->map = normalize_cache_get(se->normalize_cache,
                                           se->service, s);
            if (!sdb->map)
                return -1;
        }
    }
    return 0;
}

// called if watch should be removed because http_channel is to be destroyed
static void session_watch_cancel(void *data, struct http_channel *c,
                                 void *data2)
{
    struct session_watchentry *ent = data;

    ent->fun = 0;
    ent->data = 0;
    ent->obs = 0;
}

// set watch. Returns 0=OK, -1 if watch is already set
int session_set_watch(struct session *s, int what,
                      session_watchfun fun, void *data,
                      struct http_channel *chan)
{
    int ret;
    session_enter(s, "session_set_watch");
    if (s->watchlist[what].fun)
        ret = -1;
    else
    {

        s->watchlist[what].fun = fun;
        s->watchlist[what].data = data;
        s->watchlist[what].obs = http_add_observer(chan, &s->watchlist[what],
                                                   session_watch_cancel);
        ret = 0;
    }
    session_leave(s, "session_set_watch");
    return ret;
}

void session_alert_watch(struct session *s, int what)
{
    assert(s);
    session_enter(s, "session_alert_watch");
    if (s->watchlist[what].fun)
    {
        /* our watch is no longer associated with http_channel */
        void *data;
        session_watchfun fun;

        http_remove_observer(s->watchlist[what].obs);
        fun  = s->watchlist[what].fun;
        data = s->watchlist[what].data;

        /* reset watch before fun is invoked - in case fun wants to set
           it again */
        s->watchlist[what].fun = 0;
        s->watchlist[what].data = 0;
        s->watchlist[what].obs = 0;

        session_leave(s, "session_alert_watch");
        session_log(s, YLOG_DEBUG,
                    "Alert Watch: %d calling function: %p", what, fun);
        fun(data);
    }
    else
        session_leave(s,"session_alert_watch");
}

//callback for grep_databases
static void select_targets_callback(struct session *se,
                                    struct session_database *db)
{
    struct client *cl;
    struct client_list *l;

    for (l = se->clients_cached; l; l = l->next)
        if (client_get_database(l->client) == db)
            break;

    if (l)
        cl = l->client;
    else
    {
        cl = client_create(db->database->id);
        client_set_database(cl, db);

        l = xmalloc(sizeof(*l));
        l->client = cl;
        l->next = se->clients_cached;
        se->clients_cached = l;
    }
    /* set session always. If may be 0 if client is not active */
    client_set_session(cl, se);

    l = xmalloc(sizeof(*l));
    l->client = cl;
    l->next = se->clients_active;
    se->clients_active = l;
}

static void session_reset_active_clients(struct session *se,
                                         struct client_list *new_list)
{
    struct client_list *l;

    session_enter(se, "session_reset_active_clients");
    l = se->clients_active;
    se->clients_active = new_list;
    session_leave(se, "session_reset_active_clients");

    while (l)
    {
        struct client_list *l_next = l->next;

        client_lock(l->client);
        client_set_session(l->client, 0); /* mark client inactive */
        client_unlock(l->client);

        xfree(l);
        l = l_next;
    }
}

static void session_remove_cached_clients(struct session *se)
{
    struct client_list *l;

    session_reset_active_clients(se, 0);

    session_enter(se, "session_remove_cached_clients");
    l = se->clients_cached;
    se->clients_cached = 0;
    session_leave(se, "session_remove_cached_clients");

    while (l)
    {
        struct client_list *l_next = l->next;
        client_lock(l->client);
        client_set_session(l->client, 0);
        client_set_database(l->client, 0);
        client_unlock(l->client);
        client_destroy(l->client);
        xfree(l);
        l = l_next;
    }
}

// Associates a set of clients with a session;
// Note: Session-databases represent databases with per-session
// setting overrides
static int select_targets(struct session *se, const char *filter)
{
    return session_grep_databases(se, filter, select_targets_callback);
}

int session_active_clients(struct session *s)
{
    struct client_list *l;
    int res = 0;

    for (l = s->clients_active; l; l = l->next)
        if (client_is_active(l->client))
            res++;

    return res;
}

int session_is_preferred_clients_ready(struct session *s)
{
    struct client_list *l;
    int res = 0;

    for (l = s->clients_active; l; l = l->next)
        if (client_is_active_preferred(l->client))
            res++;
    session_log(s, YLOG_DEBUG, "Has %d active preferred clients.", res);
    return res == 0;
}

static void session_clear_set(struct session *se, struct reclist_sortparms *sp)
{
    reclist_destroy(se->reclist);
    se->reclist = 0;
    if (nmem_total(se->nmem))
        session_log(se, YLOG_DEBUG, "NMEN operation usage %zd",
                    nmem_total(se->nmem));
    nmem_reset(se->nmem);
    se->total_records = se->total_merged = 0;
    se->num_termlists = 0;

    /* reset list of sorted results and clear to relevance search */
    se->sorted_results = nmem_malloc(se->nmem, sizeof(*se->sorted_results));
    se->sorted_results->name = nmem_strdup(se->nmem, sp->name);
    se->sorted_results->increasing = sp->increasing;
    se->sorted_results->type = sp->type;
    se->sorted_results->next = 0;

    session_log(se, YLOG_DEBUG, "clear_set session_sort: field=%s increasing=%d type=%d configured",
            sp->name, sp->increasing, sp->type);

    se->reclist = reclist_create(se->nmem);
}

static void session_sort_unlocked(struct session *se, struct reclist_sortparms *sp)
{
    struct reclist_sortparms *sr;
    struct client_list *l;
    const char *field = sp->name;
    int increasing = sp->increasing;
    int type  = sp->type;
    int clients_research = 0;

    yaz_log(YLOG_LOG, "session_sort field=%s increasing=%d type=%d", field, increasing, type);
    /* see if we already have sorted for this criteria */
    for (sr = se->sorted_results; sr; sr = sr->next)
    {
        if (!reclist_sortparms_cmp(sr,sp))
            break;
    }
    if (sr)
    {
        session_log(se, YLOG_DEBUG, "search_sort: field=%s increasing=%d type=%d already fetched",
                    field, increasing, type);
        return;
    }
    session_log(se, YLOG_DEBUG, "search_sort: field=%s increasing=%d type=%d must fetch",
                    field, increasing, type);

    // We need to reset reclist on every sort that changes the records, not just for position
    // So if just one client requires new searching, we need to clear set.
    // Ask each of the client if sorting requires re-search due to native sort
    // If it does it will require us to
    for (l = se->clients_active; l; l = l->next)
    {
        struct client *cl = l->client;
        // Assume no re-search is required.
        client_parse_init(cl, 1);
        clients_research += client_parse_sort(cl, sp);
    }
    if (clients_research) {
        yaz_log(YLOG_DEBUG, "Reset results due to %d clients researching", clients_research);
        session_clear_set(se, sp);
    }
    else {
        // A new sorting based on same record set
        sr = nmem_malloc(se->nmem, sizeof(*sr));
        sr->name = nmem_strdup(se->nmem, field);
        sr->increasing = increasing;
        sr->type = type;
        sr->next = se->sorted_results;
        se->sorted_results = sr;
        session_log(se, YLOG_DEBUG, "No research/ingesting done");
        return ;
    }
    session_log(se, YLOG_DEBUG, "Re- search/ingesting for clients due to change in sort order");

    for (l = se->clients_active; l; l = l->next)
    {
        struct client *cl = l->client;
        if (client_get_state(cl) == Client_Connecting ||
            client_get_state(cl) == Client_Idle ||
            client_get_state(cl) == Client_Working) {
            client_start_search(cl);
        }
        else {
            yaz_log(YLOG_DEBUG, "Client %s: No re-start/ingest in show. Wrong client state: %d",
                        client_get_id(cl), client_get_state(cl));
        }

    }
}

void session_sort(struct session *se, struct reclist_sortparms *sp) {
    //session_enter(se, "session_sort");
    session_sort_unlocked(se, sp);
    //session_leave(se, "session_sort");
}


enum pazpar2_error_code session_search(struct session *se,
                                       const char *query,
                                       const char *startrecs,
                                       const char *maxrecs,
                                       const char *filter,
                                       const char *limit,
                                       const char **addinfo,
                                       struct reclist_sortparms *sp)
{
    int live_channels = 0;
    int no_working = 0;
    int no_failed_query = 0;
    int no_failed_limit = 0;
    struct client_list *l, *l0;
    int same_sort_order = 0;

    session_log(se, YLOG_DEBUG, "Search");

    *addinfo = 0;

    if (se->settings_modified) {
        session_remove_cached_clients(se);
    }
    else
        session_reset_active_clients(se, 0);

    session_enter(se, "session_search");
    se->settings_modified = 0;

    if (se->sorted_results) {
        if (!reclist_sortparms_cmp(se->sorted_results, sp))
            same_sort_order = 1;
    }
    session_clear_set(se, sp);
    relevance_destroy(&se->relevance);

    live_channels = select_targets(se, filter);
    if (!live_channels)
    {
        session_leave(se, "session_search");
        return PAZPAR2_NO_TARGETS;
    }

    facet_limits_destroy(se->facet_limits);
    se->facet_limits = facet_limits_create(limit);
    if (!se->facet_limits)
    {
        *addinfo = "limit";
        session_leave(se, "session_search");
        return PAZPAR2_MALFORMED_PARAMETER_VALUE;
    }

    l0 = se->clients_active;
    se->clients_active = 0;
    session_leave(se, "session_search");

    for (l = l0; l; l = l->next)
    {
        int parse_ret;
        struct client *cl = l->client;
        client_parse_init(cl, 1);
        if (prepare_map(se, client_get_database(cl)) < 0)
            continue;

        parse_ret = client_parse_query(cl, query, se->facet_limits);
        if (parse_ret == -1)
            no_failed_query++;
        else if (parse_ret == -2)
            no_failed_limit++;
        else if (parse_ret < 0)
            no_working++; /* other error, such as bad CCL map */
        else
        {
            client_parse_range(cl, startrecs, maxrecs);
            client_parse_sort(cl, sp);
            client_start_search(cl);
            no_working++;
        }
    }
    session_reset_active_clients(se, l0);

    if (no_working == 0)
    {
        if (no_failed_query > 0)
        {
            *addinfo = "query";
            return PAZPAR2_MALFORMED_PARAMETER_VALUE;
        }
        else if (no_failed_limit > 0)
        {
            *addinfo = "limit";
            return PAZPAR2_MALFORMED_PARAMETER_VALUE;
        }
        else
            return PAZPAR2_NO_TARGETS;
    }
    session_log(se, YLOG_LOG, "session_start_search done");
    return PAZPAR2_NO_ERROR;
}

// Creates a new session_database object for a database
static void session_init_databases_fun(void *context, struct database *db)
{
    struct session *se = (struct session *) context;
    struct session_database *new = nmem_malloc(se->session_nmem, sizeof(*new));
    int i;

    new->database = db;

    new->map = 0;
    assert(db->settings);
    new->settings = nmem_malloc(se->session_nmem,
                                sizeof(struct settings *) * db->num_settings);
    new->num_settings = db->num_settings;
    for (i = 0; i < db->num_settings; i++)
    {
        struct setting *setting = db->settings[i];
        new->settings[i] = setting;
    }
    new->next = se->databases;
    se->databases = new;
}

// Doesn't free memory associated with sdb -- nmem takes care of that
static void session_database_destroy(struct session_database *sdb)
{
    sdb->map = 0;
}

// Initialize session_database list -- this represents this session's view
// of the database list -- subject to modification by the settings ws command
void session_init_databases(struct session *se)
{
    se->databases = 0;
    predef_grep_databases(se, se->service, session_init_databases_fun);
}

// Probably session_init_databases_fun should be refactored instead of
// called here.
static struct session_database *load_session_database(struct session *se,
                                                      char *id)
{
    struct database *db = new_database_inherit_settings(id, se->session_nmem, se->service->settings);
    session_init_databases_fun((void*) se, db);

    // New sdb is head of se->databases list
    return se->databases;
}

// Find an existing session database. If not found, load it
static struct session_database *find_session_database(struct session *se,
                                                      char *id)
{
    struct session_database *sdb;

    for (sdb = se->databases; sdb; sdb = sdb->next)
        if (!strcmp(sdb->database->id, id))
            return sdb;
    return load_session_database(se, id);
}

// Apply a session override to a database
void session_apply_setting(struct session *se, char *dbname, char *setting,
                           char *value)
{
    struct session_database *sdb = find_session_database(se, dbname);
    struct conf_service *service = se->service;
    struct setting *new = nmem_malloc(se->session_nmem, sizeof(*new));
    int offset = settings_create_offset(service, setting);

    expand_settings_array(&sdb->settings, &sdb->num_settings, offset,
                          se->session_nmem);
    new->precedence = 0;
    new->target = dbname;
    new->name = setting;
    new->value = value;
    new->next = sdb->settings[offset];
    sdb->settings[offset] = new;

    se->settings_modified = 1;

    // Force later recompute of settings-driven data structures
    // (happens when a search starts and client connections are prepared)
    switch (offset)
    {
    case PZ_XSLT:
        if (sdb->map)
        {
            sdb->map = 0;
        }
        break;
    }
}

void session_destroy(struct session *se)
{
    struct session_database *sdb;
    session_log(se, YLOG_DEBUG, "Destroying");
    session_use(-1);
    session_remove_cached_clients(se);

    for (sdb = se->databases; sdb; sdb = sdb->next)
        session_database_destroy(sdb);
    normalize_cache_destroy(se->normalize_cache);
    relevance_destroy(&se->relevance);
    reclist_destroy(se->reclist);
    if (nmem_total(se->nmem))
        session_log(se, YLOG_DEBUG, "NMEN operation usage %zd", nmem_total(se->nmem));
    if (nmem_total(se->session_nmem))
        session_log(se, YLOG_DEBUG, "NMEN session usage %zd", nmem_total(se->session_nmem));
    facet_limits_destroy(se->facet_limits);
    nmem_destroy(se->nmem);
    service_destroy(se->service);
    yaz_mutex_destroy(&se->session_mutex);
}

size_t session_get_memory_status(struct session *session) {
    size_t session_nmem;
    if (session == 0)
        return 0;
    session_enter(session, "session_get_memory_status");
    session_nmem = nmem_total(session->nmem);
    session_leave(session, "session_get_memory_status");
    return session_nmem;
}


struct session *new_session(NMEM nmem, struct conf_service *service,
                            unsigned session_id)
{
    int i;
    struct session *session = nmem_malloc(nmem, sizeof(*session));

    char tmp_str[50];

    sprintf(tmp_str, "session#%u", session_id);

    session->session_id = session_id;
    session_log(session, YLOG_DEBUG, "New");
    session->service = service;
    session->relevance = 0;
    session->total_records = 0;
    session->number_of_warnings_unknown_elements = 0;
    session->number_of_warnings_unknown_metadata = 0;
    session->num_termlists = 0;
    session->reclist = 0;
    session->clients_active = 0;
    session->clients_cached = 0;
    session->settings_modified = 0;
    session->session_nmem = nmem;
    session->nmem = nmem_create();
    session->databases = 0;
    session->sorted_results = 0;
    session->facet_limits = 0;

    for (i = 0; i <= SESSION_WATCH_MAX; i++)
    {
        session->watchlist[i].data = 0;
        session->watchlist[i].fun = 0;
    }
    session->normalize_cache = normalize_cache_create();
    session->session_mutex = 0;
    pazpar2_mutex_create(&session->session_mutex, tmp_str);
    session_use(1);
    return session;
}

const char * client_get_suggestions_xml(struct client *cl, WRBUF wrbuf);

static struct hitsbytarget *hitsbytarget_nb(struct session *se,
                                            int *count, NMEM nmem)
{
    struct hitsbytarget *res = 0;
    struct client_list *l;
    size_t sz = 0;

    for (l = se->clients_active; l; l = l->next)
        sz++;

    res = nmem_malloc(nmem, sizeof(*res) * sz);
    *count = 0;
    for (l = se->clients_active; l; l = l->next)
    {
        struct client *cl = l->client;
        WRBUF w = wrbuf_alloc();
        const char *name = session_setting_oneval(client_get_database(cl),
                                                  PZ_NAME);

        res[*count].id = client_get_id(cl);
        res[*count].name = *name ? name : "Unknown";
        res[*count].hits = client_get_hits(cl);
        res[*count].approximation = client_get_approximation(cl);
        res[*count].records = client_get_num_records(cl);
        res[*count].filtered = client_get_num_records_filtered(cl);
        res[*count].diagnostic =
            client_get_diagnostic(cl, &res[*count].message,
                                  &res[*count].addinfo);
        res[*count].state = client_get_state_str(cl);
        res[*count].connected  = client_get_connection(cl) ? 1 : 0;
        session_settings_dump(se, client_get_database(cl), w);
        res[*count].settings_xml = nmem_strdup(nmem, wrbuf_cstr(w));
        wrbuf_rewind(w);
        wrbuf_puts(w, "");
        res[*count].suggestions_xml = nmem_strdup(nmem, client_get_suggestions_xml(cl, w));
        wrbuf_destroy(w);
        (*count)++;
    }
    return res;
}

struct hitsbytarget *get_hitsbytarget(struct session *se, int *count, NMEM nmem)
{
    struct hitsbytarget *p;
    session_enter(se, "get_hitsbytarget");
    p = hitsbytarget_nb(se, count, nmem);
    session_leave(se, "get_hitsbytarget");
    return p;
}

// Compares two hitsbytarget nodes by hitcount
static int cmp_ht(const void *p1, const void *p2)
{
    const struct hitsbytarget *h1 = p1;
    const struct hitsbytarget *h2 = p2;
    return h2->hits - h1->hits;
}

// Compares two hitsbytarget nodes by hitcount
static int cmp_ht_approx(const void *p1, const void *p2)
{
    const struct hitsbytarget *h1 = p1;
    const struct hitsbytarget *h2 = p2;
    return h2->approximation - h1->approximation;
}

static int targets_termlist_nb(WRBUF wrbuf, struct session *se, int num,
                               NMEM nmem, int version)
{
    struct hitsbytarget *ht;
    int count, i;

    ht = hitsbytarget_nb(se, &count, nmem);
    if (version >= 2)
        qsort(ht, count, sizeof(struct hitsbytarget), cmp_ht_approx);
    else
        qsort(ht, count, sizeof(struct hitsbytarget), cmp_ht);
    for (i = 0; i < count && i < num && ht[i].hits > 0; i++)
    {

        // do only print terms which have display names

        wrbuf_puts(wrbuf, "<term>\n");

        wrbuf_puts(wrbuf, "<id>");
        wrbuf_xmlputs(wrbuf, ht[i].id);
        wrbuf_puts(wrbuf, "</id>\n");

        wrbuf_puts(wrbuf, "<name>");
        if (!ht[i].name || !ht[i].name[0])
            wrbuf_xmlputs(wrbuf, "NO TARGET NAME");
        else
            wrbuf_xmlputs(wrbuf, ht[i].name);
        wrbuf_puts(wrbuf, "</name>\n");

        wrbuf_printf(wrbuf, "<frequency>" ODR_INT_PRINTF "</frequency>\n",
                     ht[i].hits);

        if (version >= 2) {
            // Should not print if we know it isn't a approximation.
            wrbuf_printf(wrbuf, "<approximation>" ODR_INT_PRINTF "</approximation>\n", ht[i].approximation);
            wrbuf_printf(wrbuf, "<records>%d</records>\n", ht[i].records - ht[i].filtered);
            wrbuf_printf(wrbuf, "<filtered>%d</filtered>\n", ht[i].filtered);
        }

        wrbuf_puts(wrbuf, "<state>");
        wrbuf_xmlputs(wrbuf, ht[i].state);
        wrbuf_puts(wrbuf, "</state>\n");

        wrbuf_printf(wrbuf, "<diagnostic>%d</diagnostic>\n",
                     ht[i].diagnostic);
        wrbuf_puts(wrbuf, "</term>\n");
    }
    return count;
}

void perform_termlist(struct http_channel *c, struct session *se,
                      const char *name, int num, int version)
{
    int i, j;
    NMEM nmem_tmp = nmem_create();
    char **names;
    int num_names = 0;

    if (!name)
        name = "*";

    nmem_strsplit(nmem_tmp, ",", name, &names, &num_names);

    session_enter(se, "perform_termlist");

    for (j = 0; j < num_names; j++)
    {
        const char *tname;
        int must_generate_empty = 1; /* bug 5350 */

        for (i = 0; i < se->num_termlists; i++)
        {
            tname = se->termlists[i].name;
            if (!strcmp(names[j], tname) || !strcmp(names[j], "*"))
            {
                struct termlist_score **p = 0;
                int len;

                wrbuf_puts(c->wrbuf, "<list name=\"");
                wrbuf_xmlputs(c->wrbuf, tname);
                wrbuf_puts(c->wrbuf, "\">\n");
                must_generate_empty = 0;

                p = termlist_highscore(se->termlists[i].termlist, &len,
                                       nmem_tmp);
                if (p)
                {
                    int i;
                    for (i = 0; i < len && i < num; i++)
                    {
                        // prevent sending empty term elements
                        if (!p[i]->display_term || !p[i]->display_term[0])
                            continue;

                        wrbuf_puts(c->wrbuf, "<term>");
                        wrbuf_puts(c->wrbuf, "<name>");
                        wrbuf_xmlputs(c->wrbuf, p[i]->display_term);
                        wrbuf_puts(c->wrbuf, "</name>");

                        wrbuf_printf(c->wrbuf,
                                     "<frequency>%d</frequency>",
                                     p[i]->frequency);
                        wrbuf_puts(c->wrbuf, "</term>\n");
                    }
                }
                wrbuf_puts(c->wrbuf, "</list>\n");
            }
        }
        tname = "xtargets";
        if (!strcmp(names[j], tname) || !strcmp(names[j], "*"))
        {
            wrbuf_puts(c->wrbuf, "<list name=\"");
            wrbuf_xmlputs(c->wrbuf, tname);
            wrbuf_puts(c->wrbuf, "\">\n");

            targets_termlist_nb(c->wrbuf, se, num, c->nmem, version);
            wrbuf_puts(c->wrbuf, "</list>\n");
            must_generate_empty = 0;
        }
        if (must_generate_empty)
        {
            wrbuf_puts(c->wrbuf, "<list name=\"");
            wrbuf_xmlputs(c->wrbuf, names[j]);
            wrbuf_puts(c->wrbuf, "\"/>\n");
        }
    }
    session_leave(se, "perform_termlist");
    nmem_destroy(nmem_tmp);
}

#ifdef MISSING_HEADERS
void report_nmem_stats(void)
{
    size_t in_use, is_free;

    nmem_get_memory_in_use(&in_use);
    nmem_get_memory_free(&is_free);

    yaz_log(YLOG_LOG, "nmem stat: use=%ld free=%ld",
            (long) in_use, (long) is_free);
}
#endif

struct record_cluster *show_single_start(struct session *se, const char *id,
                                         struct record_cluster **prev_r,
                                         struct record_cluster **next_r)
{
    struct record_cluster *r = 0;

    session_enter(se, "show_single_start");
    *prev_r = 0;
    *next_r = 0;
    if (se->reclist)
    {
        reclist_limit(se->reclist, se);

        reclist_enter(se->reclist);
        while ((r = reclist_read_record(se->reclist)))
        {
            if (!strcmp(r->recid, id))
            {
                *next_r = reclist_read_record(se->reclist);
                break;
            }
            *prev_r = r;
        }
        reclist_leave(se->reclist);
    }
    if (!r)
        session_leave(se, "show_single_start");
    return r;
}

void show_single_stop(struct session *se, struct record_cluster *rec)
{
    session_leave(se, "show_single_stop");
}


struct record_cluster **show_range_start(struct session *se,
                                         struct reclist_sortparms *sp,
                                         int start, int *num, int *total, Odr_int *sumhits, Odr_int *approx_hits)
{
    struct record_cluster **recs;
    struct reclist_sortparms *spp;
    int i;
#if USE_TIMING
    yaz_timing_t t = yaz_timing_create();
#endif
    session_enter(se, "show_range_start");
    recs = nmem_malloc(se->nmem, *num * sizeof(struct record_cluster *));
    if (!se->relevance)
    {
        *num = 0;
        *total = 0;
        *sumhits = 0;
        *approx_hits = 0;
        recs = 0;
    }
    else
    {
        struct client_list *l;

        reclist_limit(se->reclist, se);

        for (spp = sp; spp; spp = spp->next)
            if (spp->type == Metadata_sortkey_relevance)
            {
                relevance_prepare_read(se->relevance, se->reclist);
                break;
            }
        reclist_sort(se->reclist, sp);

        reclist_enter(se->reclist);
        *total = reclist_get_num_records(se->reclist);

        *sumhits = 0;
        *approx_hits = 0;
        for (l = se->clients_active; l; l = l->next) {
            *sumhits += client_get_hits(l->client);
            *approx_hits += client_get_approximation(l->client);
        }
        for (i = 0; i < start; i++)
            if (!reclist_read_record(se->reclist))
            {
                *num = 0;
                recs = 0;
                break;
            }

        for (i = 0; i < *num; i++)
        {
            struct record_cluster *r = reclist_read_record(se->reclist);
            if (!r)
            {
                *num = i;
                break;
            }
            recs[i] = r;
        }
        reclist_leave(se->reclist);
    }
#if USE_TIMING
    yaz_timing_stop(t);
    yaz_log(YLOG_LOG, "show %6.5f %3.2f %3.2f",
            yaz_timing_get_real(t), yaz_timing_get_user(t),
            yaz_timing_get_sys(t));
    yaz_timing_destroy(&t);
#endif
    return recs;
}

void show_range_stop(struct session *se, struct record_cluster **recs)
{
    session_leave(se, "show_range_stop");
}

void statistics(struct session *se, struct statistics *stat)
{
    struct client_list *l;
    int count = 0;

    memset(stat, 0, sizeof(*stat));
    stat->num_hits = 0;
    for (l = se->clients_active; l; l = l->next)
    {
        struct client *cl = l->client;
        if (!client_get_connection(cl))
            stat->num_no_connection++;
        stat->num_hits += client_get_hits(cl);
        switch (client_get_state(cl))
        {
        case Client_Connecting: stat->num_connecting++; break;
        case Client_Working: stat->num_working++; break;
        case Client_Idle: stat->num_idle++; break;
        case Client_Failed: stat->num_failed++; break;
        case Client_Error: stat->num_error++; break;
        default: break;
        }
        count++;
    }
    stat->num_records = se->total_records;

    stat->num_clients = count;
}

static struct record_metadata *record_metadata_init(
    NMEM nmem, const char *value, enum conf_metadata_type type,
    struct _xmlAttr *attr)
{
    struct record_metadata *rec_md = record_metadata_create(nmem);
    struct record_metadata_attr **attrp = &rec_md->attributes;

    for (; attr; attr = attr->next)
    {
        if (attr->children && attr->children->content)
        {
            if (strcmp((const char *) attr->name, "type"))
            {  /* skip the "type" attribute.. Its value is already part of
                  the element in output (md-%s) and so repeating it here
                  is redundant */
                *attrp = nmem_malloc(nmem, sizeof(**attrp));
                (*attrp)->name =
                    nmem_strdup(nmem, (const char *) attr->name);
                (*attrp)->value =
                    nmem_strdup(nmem, (const char *) attr->children->content);
                attrp = &(*attrp)->next;
            }
        }
    }
    *attrp = 0;

    if (type == Metadata_type_generic)
    {
        char *p = nmem_strdup(nmem, value);

        p = normalize7bit_generic(p, " ,/.:([");

        rec_md->data.text.disp = p;
        rec_md->data.text.sort = 0;
    }
    else if (type == Metadata_type_year || type == Metadata_type_date)
    {
        int first, last;
        int longdate = 0;

        if (type == Metadata_type_date)
            longdate = 1;
        if (extract7bit_dates((char *) value, &first, &last, longdate) < 0)
            return 0;

        rec_md->data.number.min = first;
        rec_md->data.number.max = last;
    }
    else
        return 0;
    return rec_md;
}

static int get_mergekey_from_doc(xmlDoc *doc, xmlNode *root, const char *name,
                                 struct conf_service *service, WRBUF norm_wr)
{
    xmlNode *n;
    int no_found = 0;
    for (n = root->children; n; n = n->next)
    {
        if (n->type != XML_ELEMENT_NODE)
            continue;
        if (!strcmp((const char *) n->name, "metadata"))
        {
            xmlChar *type = xmlGetProp(n, (xmlChar *) "type");
            if (type == NULL) {
                yaz_log(YLOG_FATAL, "Missing type attribute on metadata element. Skipping!");
            }
            else if (!strcmp(name, (const char *) type))
            {
                xmlChar *value = xmlNodeListGetString(doc, n->children, 1);
                if (value)
                {
                    const char *norm_str;
                    pp2_charset_token_t prt =
                        pp2_charset_token_create(service->charsets, "mergekey");

                    pp2_charset_token_first(prt, (const char *) value, 0);
                    if (wrbuf_len(norm_wr) > 0)
                        wrbuf_puts(norm_wr, " ");
                    wrbuf_puts(norm_wr, name);
                    while ((norm_str =
                            pp2_charset_token_next(prt)))
                    {
                        if (*norm_str)
                        {
                            wrbuf_puts(norm_wr, " ");
                            wrbuf_puts(norm_wr, norm_str);
                        }
                    }
                    xmlFree(value);
                    pp2_charset_token_destroy(prt);
                    no_found++;
                }
            }
            xmlFree(type);
        }
    }
    return no_found;
}

static const char *get_mergekey(xmlDoc *doc, struct client *cl, int record_no,
                                struct conf_service *service, NMEM nmem)
{
    char *mergekey_norm = 0;
    xmlNode *root = xmlDocGetRootElement(doc);
    WRBUF norm_wr = wrbuf_alloc();

    /* consider mergekey from XSL first */
    xmlChar *mergekey = xmlGetProp(root, (xmlChar *) "mergekey");
    if (mergekey)
    {
        const char *norm_str;
        pp2_charset_token_t prt =
            pp2_charset_token_create(service->charsets, "mergekey");

        pp2_charset_token_first(prt, (const char *) mergekey, 0);
        while ((norm_str = pp2_charset_token_next(prt)))
        {
            if (*norm_str)
            {
                if (wrbuf_len(norm_wr))
                    wrbuf_puts(norm_wr, " ");
                wrbuf_puts(norm_wr, norm_str);
            }
        }
        pp2_charset_token_destroy(prt);
        xmlFree(mergekey);
    }
    else
    {
        /* no mergekey defined in XSL. Look for mergekey metadata instead */
        int field_id;
        for (field_id = 0; field_id < service->num_metadata; field_id++)
        {
            struct conf_metadata *ser_md = &service->metadata[field_id];
            if (ser_md->mergekey != Metadata_mergekey_no)
            {
                int r = get_mergekey_from_doc(doc, root, ser_md->name,
                                              service, norm_wr);
                if (r == 0 && ser_md->mergekey == Metadata_mergekey_required)
                {
                    /* no mergekey on this one and it is required..
                       Generate unique key instead */
                    wrbuf_rewind(norm_wr);
                    break;
                }
            }
        }
    }

    /* generate unique key if none is not generated already or is empty */
    if (wrbuf_len(norm_wr) == 0)
    {
        wrbuf_printf(norm_wr, "position: %s-%d",
                     client_get_id(cl), record_no);
    }
    else
    {
        const char *lead = "content: ";
        wrbuf_insert(norm_wr, 0, lead, strlen(lead));
    }
    if (wrbuf_len(norm_wr) > 0)
        mergekey_norm = nmem_strdup(nmem, wrbuf_cstr(norm_wr));
    wrbuf_destroy(norm_wr);
    return mergekey_norm;
}

/** \brief see if metadata for pz:recordfilter exists
    \param root xml root element of normalized record
    \param sdb session database for client
    \retval 0 if there is no metadata for pz:recordfilter
    \retval 1 if there is metadata for pz:recordfilter

    If there is no pz:recordfilter defined, this function returns 1
    as well.
*/

static int check_record_filter(xmlNode *root, struct session_database *sdb)
{
    int match = 0;
    xmlNode *n;
    const char *s;
    s = session_setting_oneval(sdb, PZ_RECORDFILTER);

    if (!s || !*s)
        return 1;

    for (n = root->children; n; n = n->next)
    {
        if (n->type != XML_ELEMENT_NODE)
            continue;
        if (!strcmp((const char *) n->name, "metadata"))
        {
            xmlChar *type = xmlGetProp(n, (xmlChar *) "type");
            if (type)
            {
                size_t len;
        int substring;
                const char *eq;

                if ((eq = strchr(s, '=')))
            substring = 0;
        else if ((eq = strchr(s, '~')))
            substring = 1;
        if (eq)
            len = eq - s;
                else
                    len = strlen(s);
                if (len == strlen((const char *)type) &&
                    !memcmp((const char *) type, s, len))
                {
                    xmlChar *value = xmlNodeGetContent(n);
                    if (value && *value)
                    {
                        if (!eq ||
                (substring && strstr((const char *) value, eq+1)) ||
                (!substring && !strcmp((const char *) value, eq + 1)))
                            match = 1;
                    }
                    xmlFree(value);
                }
                xmlFree(type);
            }
        }
    }
    return match;
}


static int ingest_to_cluster(struct client *cl,
                             xmlDoc *xdoc,
                             xmlNode *root,
                             int record_no,
                             const char *mergekey_norm);

/** \brief ingest XML record
    \param cl client holds the result set for record
    \param rec record buffer (0 terminated)
    \param record_no record position (1, 2, ..)
    \param nmem working NMEM
    \retval 0 OK
    \retval -1 failure
    \retval -2 Filtered
*/
int ingest_record(struct client *cl, const char *rec,
                  int record_no, NMEM nmem)
{
    struct session *se = client_get_session(cl);
    int ret = 0;
    struct session_database *sdb = client_get_database(cl);
    struct conf_service *service = se->service;
    xmlDoc *xdoc = normalize_record(se, sdb, service, rec, nmem);
    xmlNode *root;
    const char *mergekey_norm;

    if (!xdoc)
        return -1;

    root = xmlDocGetRootElement(xdoc);

    if (!check_record_filter(root, sdb))
    {
        session_log(se, YLOG_LOG, "Filtered out record no %d from %s", record_no, sdb->database->id);
        xmlFreeDoc(xdoc);
        return -2;
    }

    mergekey_norm = get_mergekey(xdoc, cl, record_no, service, nmem);
    if (!mergekey_norm)
    {
        session_log(se, YLOG_WARN, "Got no mergekey");
        xmlFreeDoc(xdoc);
        return -1;
    }
    session_enter(se, "ingest_record");
    if (client_get_session(cl) == se)
        ret = ingest_to_cluster(cl, xdoc, root, record_no, mergekey_norm);
    session_leave(se, "ingest_record");

    xmlFreeDoc(xdoc);
    return ret;
}

//    struct conf_metadata *ser_md = &service->metadata[md_field_id];
//    struct record_metadata *rec_md = record->metadata[md_field_id];
static int match_metadata_local(struct conf_metadata *ser_md,
                                struct record_metadata *rec_md0,
                                char **values, int num_v)
{
    int i;
    struct record_metadata *rec_md = rec_md0;
    for (i = 0; i < num_v; )
    {
        if (rec_md)
        {
            if (ser_md->type == Metadata_type_year
                || ser_md->type == Metadata_type_date)
            {
                int y = atoi(values[i]);
                if (y >= rec_md->data.number.min
                    && y <= rec_md->data.number.max)
                    break;
            }
            else
            {
                yaz_log(YLOG_DEBUG, "cmp: '%s' '%s'", rec_md->data.text.disp, values[i]);
                if (!strcmp(rec_md->data.text.disp, values[i]))
                {
                    // Value equals, should not be filtered.
                    break;
                }
            }
            rec_md = rec_md->next;
        }
        else
        {
            rec_md = rec_md0;
            i++;
        }
    }
    return i < num_v ? 1 : 0;
}

int session_check_cluster_limit(struct session *se, struct record_cluster *rec)
{
    int i;
    struct conf_service *service = se->service;
    int ret = 1;
    const char *name;
    const char *value;
    NMEM nmem_tmp = nmem_create();

    for (i = 0; (name = facet_limits_get(se->facet_limits, i, &value)); i++)
    {
        int j;
        for (j = 0; j < service->num_metadata; j++)
        {
            struct conf_metadata *md = service->metadata + j;
            if (!strcmp(md->name, name) && md->limitcluster)
            {
                char **values = 0;
                int num = 0;
                int md_field_id =
                    conf_service_metadata_field_id(service,
                                                   md->limitcluster);

                if (md_field_id < 0)
                {
                    ret = 0;
                    break;
                }

                nmem_strsplit_escape2(nmem_tmp, "|", value, &values,
                                      &num, 1, '\\', 1);

                if (!match_metadata_local(&service->metadata[md_field_id],
                                          rec->metadata[md_field_id],
                                          values, num))
                {
                    ret = 0;
                    break;
                }
            }
        }
    }
    nmem_destroy(nmem_tmp);
    return ret;
}

// Skip record on non-zero
static int check_limit_local(struct client *cl,
                             struct record *record,
                             int record_no)
{
    int skip_record = 0;
    struct session *se = client_get_session(cl);
    struct conf_service *service = se->service;
    NMEM nmem_tmp = nmem_create();
    struct session_database *sdb = client_get_database(cl);
    int l = 0;
    while (!skip_record)
    {
        int md_field_id;
        char **values = 0;
        int num_v = 0;
        const char *name =
            client_get_facet_limit_local(cl, sdb, &l, nmem_tmp,
                                         &num_v, &values);
        if (!name)
            break;

        if (!strcmp(name, "*"))
        {
            for (md_field_id = 0; md_field_id < service->num_metadata;
                 md_field_id++)
            {
                if (match_metadata_local(
                        &service->metadata[md_field_id],
                        record->metadata[md_field_id],
                        values, num_v))
                    break;
            }
            if (md_field_id == service->num_metadata)
                skip_record = 1;
        }
        else
        {
            md_field_id = conf_service_metadata_field_id(service, name);
            if (md_field_id < 0)
            {
                skip_record = 1;
                break;
            }
            if (!match_metadata_local(
                    &service->metadata[md_field_id],
                    record->metadata[md_field_id],
                    values, num_v))
            {
                skip_record = 1;
            }
        }
    }
    nmem_destroy(nmem_tmp);
    return skip_record;
}

static int ingest_to_cluster(struct client *cl,
                             xmlDoc *xdoc,
                             xmlNode *root,
                             int record_no,
                             const char *mergekey_norm)
{
    xmlNode *n;
    xmlChar *type = 0;
    xmlChar *value = 0;
    struct session *se = client_get_session(cl);
    struct conf_service *service = se->service;
    int term_factor = 1;
    struct record_cluster *cluster;
    struct session_database *sdb = client_get_database(cl);
    struct record *record = record_create(se->nmem,
                                          service->num_metadata,
                                          service->num_sortkeys, cl,
                                          record_no);

    for (n = root->children; n; n = n->next)
    {
        if (type)
            xmlFree(type);
        if (value)
            xmlFree(value);
        type = value = 0;

        if (n->type != XML_ELEMENT_NODE)
            continue;
        if (!strcmp((const char *) n->name, "metadata"))
        {
            struct conf_metadata *ser_md = 0;
            struct record_metadata **wheretoput = 0;
            struct record_metadata *rec_md = 0;
            int md_field_id = -1;

            type = xmlGetProp(n, (xmlChar *) "type");
            value = xmlNodeListGetString(xdoc, n->children, 1);

            if (!type || !value || !*value)
                continue;

            md_field_id
                = conf_service_metadata_field_id(service, (const char *) type);
            if (md_field_id < 0)
            {
                if (se->number_of_warnings_unknown_metadata == 0)
                {
                    session_log(se, YLOG_WARN,
                            "Ignoring unknown metadata element: %s", type);
                }
                se->number_of_warnings_unknown_metadata++;
                continue;
            }

            ser_md = &service->metadata[md_field_id];

            // non-merged metadata
            rec_md = record_metadata_init(se->nmem, (const char *) value,
                                          ser_md->type, n->properties);
            if (!rec_md)
            {
                session_log(se, YLOG_WARN, "bad metadata data '%s' "
                            "for element '%s'", value, type);
                continue;
            }
            wheretoput = &record->metadata[md_field_id];
            while (*wheretoput)
                wheretoput = &(*wheretoput)->next;
            *wheretoput = rec_md;
        }
    }

    if (check_limit_local(cl, record, record_no))
    {
        session_log(se, YLOG_LOG, "Facet filtered out record no %d from %s",
                    record_no, sdb->database->id);
        if (type)
            xmlFree(type);
        if (value)
            xmlFree(value);
        return -2;
    }
    cluster = reclist_insert(se->reclist, service, record,
                             mergekey_norm, &se->total_merged);
    if (!cluster)
        return -1;

    {
        const char *use_term_factor_str =
            session_setting_oneval(sdb, PZ_TERMLIST_TERM_FACTOR);
        if (use_term_factor_str && use_term_factor_str[0] == '1')
        {
            int maxrecs = client_get_maxrecs(cl);
            int hits = (int) client_get_hits(cl);
            term_factor = MAX(hits, maxrecs) /  MAX(1, maxrecs);
            assert(term_factor >= 1);
            yaz_log(YLOG_DEBUG, "Using term factor: %d (%d / %d)", term_factor, MAX(hits, maxrecs), MAX(1, maxrecs));
        }
    }

    if (global_parameters.dump_records)
        session_log(se, YLOG_LOG, "Cluster id %s from %s (#%d)", cluster->recid,
                    sdb->database->id, record_no);


    relevance_newrec(se->relevance, cluster);

    // now parsing XML record and adding data to cluster or record metadata
    for (n = root->children; n; n = n->next)
    {
        pp2_charset_token_t prt;
        if (type)
            xmlFree(type);
        if (value)
            xmlFree(value);
        type = value = 0;

        if (n->type != XML_ELEMENT_NODE)
            continue;
        if (!strcmp((const char *) n->name, "metadata"))
        {
            struct conf_metadata *ser_md = 0;
            struct conf_sortkey *ser_sk = 0;
            struct record_metadata **wheretoput = 0;
            struct record_metadata *rec_md = 0;
            int md_field_id = -1;
            int sk_field_id = -1;
            const char *rank;
            xmlChar *xml_rank;

            type = xmlGetProp(n, (xmlChar *) "type");
            value = xmlNodeListGetString(xdoc, n->children, 1);

            if (!type || !value || !*value)
                continue;

            md_field_id
                = conf_service_metadata_field_id(service, (const char *) type);
            if (md_field_id < 0)
                continue;

            ser_md = &service->metadata[md_field_id];

            if (ser_md->sortkey_offset >= 0)
            {
                sk_field_id = ser_md->sortkey_offset;
                ser_sk = &service->sortkeys[sk_field_id];
            }

            // merged metadata
            rec_md = record_metadata_init(se->nmem, (const char *) value,
                                          ser_md->type, 0);
            if (!rec_md)
                continue;

            xml_rank = xmlGetProp(n, (xmlChar *) "rank");
            rank = xml_rank ? (const char *) xml_rank : ser_md->rank;

            wheretoput = &cluster->metadata[md_field_id];

            // and polulate with data:
            // assign cluster or record based on merge action
            if (ser_md->merge == Metadata_merge_unique)
            {
                while (*wheretoput)
                {
                    if (!strcmp((const char *) (*wheretoput)->data.text.disp,
                                rec_md->data.text.disp))
                        break;
                    wheretoput = &(*wheretoput)->next;
                }
                if (!*wheretoput)
                    *wheretoput = rec_md;
            }
            else if (ser_md->merge == Metadata_merge_longest)
            {
                if (!*wheretoput
                    || strlen(rec_md->data.text.disp)
                    > strlen((*wheretoput)->data.text.disp))
                {
                    *wheretoput = rec_md;
                    if (ser_sk)
                    {
                        const char *sort_str = 0;
                        int skip_article =
                            ser_sk->type == Metadata_sortkey_skiparticle;

                        if (!cluster->sortkeys[sk_field_id])
                            cluster->sortkeys[sk_field_id] =
                                nmem_malloc(se->nmem,
                                            sizeof(union data_types));

                        prt =
                            pp2_charset_token_create(service->charsets, "sort");

                        pp2_charset_token_first(prt, rec_md->data.text.disp,
                                                skip_article);

                        pp2_charset_token_next(prt);

                        sort_str = pp2_get_sort(prt);

                        cluster->sortkeys[sk_field_id]->text.disp =
                            rec_md->data.text.disp;
                        if (!sort_str)
                        {
                            sort_str = rec_md->data.text.disp;
                            session_log(se, YLOG_WARN,
                                    "Could not make sortkey. Bug #1858");
                        }
                        cluster->sortkeys[sk_field_id]->text.sort =
                            nmem_strdup(se->nmem, sort_str);
                        pp2_charset_token_destroy(prt);
                    }
                }
            }
            else if (ser_md->merge == Metadata_merge_all)
            {
                while (*wheretoput)
                    wheretoput = &(*wheretoput)->next;
                *wheretoput = rec_md;
            }
            else if (ser_md->merge == Metadata_merge_range)
            {
                if (!*wheretoput)
                {
                    *wheretoput = rec_md;
                    if (ser_sk)
                        cluster->sortkeys[sk_field_id]
                            = &rec_md->data;
                }
                else
                {
                    int this_min = rec_md->data.number.min;
                    int this_max = rec_md->data.number.max;
                    if (this_min < (*wheretoput)->data.number.min)
                        (*wheretoput)->data.number.min = this_min;
                    if (this_max > (*wheretoput)->data.number.max)
                        (*wheretoput)->data.number.max = this_max;
                }
            }

            // ranking of _all_ fields enabled ...
            if (rank)
            {
                relevance_countwords(se->relevance, cluster,
                                     (char *) value, rank, ser_md->name);
            }

            // construct facets ... unless the client already has reported them
            if (ser_md->termlist && !client_has_facet(cl, (char *) type))
            {
                if (ser_md->type == Metadata_type_year)
                {
                    char year[64];
                    sprintf(year, "%d", rec_md->data.number.max);

                    add_facet(se, (char *) type, year, term_factor);
                    if (rec_md->data.number.max != rec_md->data.number.min)
                    {
                        sprintf(year, "%d", rec_md->data.number.min);
                        add_facet(se, (char *) type, year, term_factor);
                    }
                }
                else
                    add_facet(se, (char *) type, (char *) value, term_factor);
            }

            // cleaning up
            if (xml_rank)
                xmlFree(xml_rank);
            xmlFree(type);
            xmlFree(value);
            type = value = 0;
        }
        else
        {
            if (se->number_of_warnings_unknown_elements == 0)
                session_log(se, YLOG_WARN,
                        "Unexpected element in internal record: %s", n->name);
            se->number_of_warnings_unknown_elements++;
        }
    }
    if (type)
        xmlFree(type);
    if (value)
        xmlFree(value);

    relevance_donerecord(se->relevance, cluster);
    se->total_records++;

    return 0;
}

void session_log(struct session *s, int level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);

    yaz_vsnprintf(buf, sizeof(buf)-30, fmt, ap);
    yaz_log(level, "Session %u: %s", s->session_id, buf);

    va_end(ap);
}

struct record *session_get_ingested(struct session *s)
{
    return reclist_get_ingested(s->reclist);
}


/*
 * Local variables:
 * c-basic-offset: 4
 * c-file-style: "Stroustrup"
 * indent-tabs-mode: nil
 * End:
 * vim: shiftwidth=4 tabstop=8 expandtab
 */

