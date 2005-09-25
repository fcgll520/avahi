/* $Id$ */

/***
  This file is part of avahi.
 
  avahi is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  avahi is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
  Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with avahi; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <avahi-common/malloc.h>

#include "server.h"
#include "browse.h"
#include "socket.h"
#include "log.h"
#include "hashmap.h"
#include "wide-area.h"

#define MAX_CACHE_ENTRIES 500

typedef struct AvahiWideAreaCacheEntry AvahiWideAreaCacheEntry;

struct AvahiWideAreaCacheEntry {
    AvahiWideAreaLookupEngine *engine;
    
    AvahiRecord *record;
    struct timeval timestamp;
    struct timeval expiry;

    AvahiTimeEvent *time_event;
    
    AVAHI_LLIST_FIELDS(AvahiWideAreaCacheEntry, by_key);
    AVAHI_LLIST_FIELDS(AvahiWideAreaCacheEntry, cache);
};

struct AvahiWideAreaLookup {
    AvahiWideAreaLookupEngine *engine;
    int dead;
    
    uint32_t id;  /* effectively just an uint16_t, but we need it as an index for a hash table */
    AvahiTimeEvent *time_event;

    AvahiKey *key, *cname_key;
    
    int n_send;
    AvahiDnsPacket *packet;

    AvahiWideAreaLookupCallback callback;
    void *userdata;

    AvahiAddress dns_server_used;

    AVAHI_LLIST_FIELDS(AvahiWideAreaLookup, lookups);
    AVAHI_LLIST_FIELDS(AvahiWideAreaLookup, by_key);
};

struct AvahiWideAreaLookupEngine {
    AvahiServer *server;

    int fd_ipv4, fd_ipv6;
    AvahiWatch *watch_ipv4, *watch_ipv6;

    uint16_t next_id;
    
    /* Cache */
    AVAHI_LLIST_HEAD(AvahiWideAreaCacheEntry, cache);
    AvahiHashmap *cache_by_key;
    unsigned cache_n_entries;

    /* Lookups */
    AVAHI_LLIST_HEAD(AvahiWideAreaLookup, lookups);
    AvahiHashmap *lookups_by_id;
    AvahiHashmap *lookups_by_key;

    int cleanup_dead;

    AvahiAddress dns_servers[AVAHI_MAX_WIDE_AREA_SERVERS];
    unsigned n_dns_servers;
    unsigned current_dns_server;
};

static AvahiWideAreaLookup* find_lookup(AvahiWideAreaLookupEngine *e, uint16_t id) {
    AvahiWideAreaLookup *l;
    int i = (int) id;
    
    assert(e);

    if (!(l = avahi_hashmap_lookup(e->lookups_by_id, &i)))
        return NULL;
    
    assert(l->id == id);

    if (l->dead)
        return NULL;
    
    return l;
}

static int send_to_dns_server(AvahiWideAreaLookupEngine *e, AvahiDnsPacket *p) {
    AvahiAddress *a;
    
    assert(e);
    assert(p);

    if (e->n_dns_servers <= 0)
        return -1;

    assert(e->current_dns_server < e->n_dns_servers);

    a = &e->dns_servers[e->current_dns_server];
    
    if (a->proto == AVAHI_PROTO_INET) {

        if (e->fd_ipv4 < 0)
            return -1;
        
        return avahi_send_dns_packet_ipv4(e->fd_ipv4, AVAHI_IF_UNSPEC, p, &a->data.ipv4, AVAHI_DNS_PORT);
        
    } else {
        assert(a->proto == AVAHI_PROTO_INET6);

        if (e->fd_ipv6 < 0)
            return -1;
        
        return avahi_send_dns_packet_ipv6(e->fd_ipv6, AVAHI_IF_UNSPEC, p, &a->data.ipv6, AVAHI_DNS_PORT);
    }
}

static void next_dns_server(AvahiWideAreaLookupEngine *e) {
    assert(e);

    e->current_dns_server++;

    if (e->current_dns_server >= e->n_dns_servers)
        e->current_dns_server = 0;
}

static void sender_timeout_callback(AvahiTimeEvent *e, void *userdata) {
    AvahiWideAreaLookup *l = userdata;
    struct timeval tv;

    assert(l);

    /* Try another DNS server after three retries */
    if (l->n_send >= 3 && avahi_address_cmp(&l->engine->dns_servers[l->engine->current_dns_server], &l->dns_server_used) == 0) {
        next_dns_server(l->engine);

        if (avahi_address_cmp(&l->engine->dns_servers[l->engine->current_dns_server], &l->dns_server_used) == 0)
            /* There is no other DNS server, fail */
            l->n_send = 1000;
    }
    
    if (l->n_send >= 6) {
        avahi_log_warn(__FILE__": Query timed out.");
        l->callback(l->engine, AVAHI_BROWSER_FAILURE, AVAHI_LOOKUP_RESULT_WIDE_AREA, NULL, l->userdata);
        avahi_wide_area_lookup_free(l);
        return;
    }

    assert(l->packet);
    send_to_dns_server(l->engine, l->packet);
    l->n_send++;

    avahi_time_event_update(e, avahi_elapse_time(&tv, 1000, 0));
}

AvahiWideAreaLookup *avahi_wide_area_lookup_new(
    AvahiWideAreaLookupEngine *e,
    AvahiKey *key,
    AvahiWideAreaLookupCallback callback,
    void *userdata) {
    
    struct timeval tv;
    AvahiWideAreaLookup *l, *t;
    uint8_t *p;

    assert(e);
    assert(key);
    assert(callback);
    assert(userdata);

    l = avahi_new(AvahiWideAreaLookup, 1);
    l->engine = e;
    l->dead = 0;
    l->key = avahi_key_ref(key);
    l->cname_key = avahi_key_new_cname(l->key);
    l->callback = callback;
    l->userdata = userdata;

    /* If more than 65K wide area quries are issued simultaneously,
     * this will break. This should be limited by some higher level */

    for (;; e->next_id++)
        if (!find_lookup(e, e->next_id))
            break; /* This ID is not yet used. */

    l->id = e->next_id++;
    
    /* We keep the packet around in case we need to repeat our query */
    l->packet = avahi_dns_packet_new(0);

    avahi_dns_packet_set_field(l->packet, AVAHI_DNS_FIELD_ID, (uint16_t) l->id);
    avahi_dns_packet_set_field(l->packet, AVAHI_DNS_FIELD_FLAGS, AVAHI_DNS_FLAGS(0, 0, 0, 0, 1, 0, 0, 0, 0, 0));

    p = avahi_dns_packet_append_key(l->packet, key, 0);
    assert(p);
    
    avahi_dns_packet_set_field(l->packet, AVAHI_DNS_FIELD_QDCOUNT, 1);

    if (send_to_dns_server(e, l->packet) < 0) {
        avahi_log_error(__FILE__": Failed to send packet.");
        avahi_dns_packet_free(l->packet);
        avahi_key_unref(l->key);
        if (l->cname_key)
            avahi_key_unref(l->cname_key);
        avahi_free(l);
        return NULL;
    }

    l->n_send = 1;
    
    l->time_event = avahi_time_event_new(e->server->time_event_queue, avahi_elapse_time(&tv, 500, 0), sender_timeout_callback, l);

    avahi_hashmap_insert(e->lookups_by_id, &l->id, l);

    t = avahi_hashmap_lookup(e->lookups_by_key, l->key);
    AVAHI_LLIST_PREPEND(AvahiWideAreaLookup, by_key, t, l);
    avahi_hashmap_replace(e->lookups_by_key, avahi_key_ref(l->key), t);

    AVAHI_LLIST_PREPEND(AvahiWideAreaLookup, lookups, e->lookups, l);
    
    return l;
}

static void lookup_stop(AvahiWideAreaLookup *l) {
    assert(l);
    
    l->callback = NULL;

    if (l->time_event) {
        avahi_time_event_free(l->time_event);
        l->time_event = NULL;
    }
}

static void lookup_destroy(AvahiWideAreaLookup *l) {
    AvahiWideAreaLookup *t;
    assert(l);
    
    lookup_stop(l);

    t = avahi_hashmap_lookup(l->engine->lookups_by_key, l->key);
    AVAHI_LLIST_REMOVE(AvahiWideAreaLookup, by_key, t, l);
    if (t)
        avahi_hashmap_replace(l->engine->lookups_by_key, avahi_key_ref(l->key), t);
    else
        avahi_hashmap_remove(l->engine->lookups_by_key, l->key);

    AVAHI_LLIST_REMOVE(AvahiWideAreaLookup, lookups, l->engine->lookups, l);
    
    avahi_hashmap_remove(l->engine->lookups_by_id, &l->id);
    avahi_dns_packet_free(l->packet);

    if (l->key)
        avahi_key_unref(l->key);

    if (l->cname_key)
        avahi_key_unref(l->cname_key);
    
    avahi_free(l);
}

void avahi_wide_area_lookup_free(AvahiWideAreaLookup *l) {
    assert(l);

    if (l->dead)
        return;

    l->dead = 1;
    l->engine->cleanup_dead = 1;
    lookup_stop(l);
}

void avahi_wide_area_cleanup(AvahiWideAreaLookupEngine *e) {
    AvahiWideAreaLookup *l, *n;
    assert(e);

    while (e->cleanup_dead) {
        e->cleanup_dead = 0;
    
        for (l = e->lookups; l; l = n) {
            n = l->lookups_next;
            
            if (l->dead)
                lookup_destroy(l);
        }
    }
}

static void cache_entry_free(AvahiWideAreaCacheEntry *c) {
    AvahiWideAreaCacheEntry *t;
    assert(c);

    if (c->time_event)
        avahi_time_event_free(c->time_event);

    AVAHI_LLIST_REMOVE(AvahiWideAreaCacheEntry, cache, c->engine->cache, c);

    t = avahi_hashmap_lookup(c->engine->cache_by_key, c->record->key);
    AVAHI_LLIST_REMOVE(AvahiWideAreaCacheEntry, by_key, t, c);
    if (t)
        avahi_hashmap_replace(c->engine->cache_by_key, avahi_key_ref(c->record->key), t);
    else
        avahi_hashmap_remove(c->engine->cache_by_key, c->record->key);

    c->engine->cache_n_entries --;

    avahi_record_unref(c->record);
    avahi_free(c);
}

static void expiry_event(AvahiTimeEvent *te, void *userdata) {
    AvahiWideAreaCacheEntry *e = userdata;
    
    assert(te);
    assert(e);

    cache_entry_free(e);
}

static AvahiWideAreaCacheEntry* find_record_in_cache(AvahiWideAreaLookupEngine *e, AvahiRecord *r) {
    AvahiWideAreaCacheEntry *c;
    
    assert(e);
    assert(r);

    for (c = avahi_hashmap_lookup(e->cache_by_key, r->key); c; c = c->by_key_next)
        if (avahi_record_equal_no_ttl(r, c->record))
            return c;

    return NULL;
}

static void run_callbacks(AvahiWideAreaLookupEngine *e, AvahiRecord *r) {
    AvahiWideAreaLookup *l;
    
    assert(e);
    assert(r);

    for (l = avahi_hashmap_lookup(e->lookups_by_key, r->key); l; l = l->by_key_next) {
        if (l->dead || !l->callback)
            continue;
        
        l->callback(e, AVAHI_BROWSER_NEW, AVAHI_LOOKUP_RESULT_WIDE_AREA, r, l->userdata);
    }
    
    if (r->key->clazz == AVAHI_DNS_CLASS_IN && r->key->type == AVAHI_DNS_TYPE_CNAME) {
        /* It's a CNAME record, so we have to scan the all lookups to see if one matches */

        for (l = e->lookups; l; l = l->lookups_next) {
            AvahiKey *key;

            if (l->dead || !l->callback)
                continue;
            
            if ((key = avahi_key_new_cname(l->key))) {
                if (avahi_key_equal(r->key, key))
                    l->callback(e, AVAHI_BROWSER_NEW, AVAHI_LOOKUP_RESULT_WIDE_AREA, r, l->userdata);

                avahi_key_unref(key);
            }
        }
    }
}

static void add_to_cache(AvahiWideAreaLookupEngine *e, AvahiRecord *r) {
    AvahiWideAreaCacheEntry *c;
    int is_new;
    
    assert(e);
    assert(r);

    if ((c = find_record_in_cache(e, r))) {
        is_new = 0;

        /* Update the existing entry */
        avahi_record_unref(c->record);
    } else {
        AvahiWideAreaCacheEntry *t;

        is_new = 1;

        /* Enforce cache size */
        if (e->cache_n_entries >= MAX_CACHE_ENTRIES)
            /* Eventually we should improve the caching algorithm here */
            goto finish;
        
        c = avahi_new(AvahiWideAreaCacheEntry, 1);
        c->engine = e;
        c->time_event = NULL;

        AVAHI_LLIST_PREPEND(AvahiWideAreaCacheEntry, cache, e->cache, c);

        /* Add the new entry to the cache entry hash table */
        t = avahi_hashmap_lookup(e->cache_by_key, r->key);
        AVAHI_LLIST_PREPEND(AvahiWideAreaCacheEntry, by_key, t, c);
        avahi_hashmap_replace(e->cache_by_key, avahi_key_ref(r->key), t);

        e->cache_n_entries ++;
    }

    c->record = avahi_record_ref(r);
    
    gettimeofday(&c->timestamp, NULL);
    c->expiry = c->timestamp;
    avahi_timeval_add(&c->expiry, r->ttl * 1000000);

    if (c->time_event)
        avahi_time_event_update(c->time_event, &c->expiry);
    else
        c->time_event = avahi_time_event_new(e->server->time_event_queue, &c->expiry, expiry_event, c);

finish:
    
    if (is_new)
        run_callbacks(e, r);
}

static void handle_packet(AvahiWideAreaLookupEngine *e, AvahiDnsPacket *p, AvahiAddress *a) {
    AvahiWideAreaLookup *l = NULL;
    int i, r;
    
    assert(e);
    assert(p);

    if (avahi_dns_packet_check_valid(p) < 0 || avahi_dns_packet_is_query(p)) {
        avahi_log_warn(__FILE__": Ignoring invalid response for wide area datagram.");
        goto finish;
    }

/*     avahi_log_debug(__FILE__": Recieving unicast packet %u", avahi_dns_packet_get_field(p, AVAHI_DNS_FIELD_ID)); */
    
    if (!(l = find_lookup(e, avahi_dns_packet_get_field(p, AVAHI_DNS_FIELD_ID)))) {
/*         avahi_log_warn(__FILE__": Got response for query we didn't send."); */
        goto finish;
    }


    if ((r = avahi_dns_packet_get_field(p, AVAHI_DNS_FIELD_FLAGS) & 15) != 0) {
/*         avahi_log_debug(__FILE__": Response failed for wide area datagram: %i", r); */

        /* Tell the user about the failure */
        if (l->callback)
            l->callback(e, r == 3 ? AVAHI_BROWSER_NOT_FOUND : AVAHI_BROWSER_FAILURE, AVAHI_LOOKUP_RESULT_WIDE_AREA, NULL, l->userdata);
        goto finish;
    }

    /* Skip over the question */
    for (i = (int) avahi_dns_packet_get_field(p, AVAHI_DNS_FIELD_QDCOUNT); i > 0; i--) {
        AvahiKey *k;
        
        if (!(k = avahi_dns_packet_consume_key(p, NULL))) {
            avahi_log_warn(__FILE__": Wide area response packet too short.");
            goto finish;
        }

        avahi_key_unref(k);
    }

    for (i = (int) avahi_dns_packet_get_field(p, AVAHI_DNS_FIELD_ANCOUNT) +
             (int) avahi_dns_packet_get_field(p, AVAHI_DNS_FIELD_NSCOUNT) +
             (int) avahi_dns_packet_get_field(p, AVAHI_DNS_FIELD_ARCOUNT); i > 0; i--) {

        AvahiRecord *rr;

        if (!(rr = avahi_dns_packet_consume_record(p, NULL))) {
            avahi_log_warn(__FILE__": Wide area response packet too short (2).");
            goto finish;
        }

        add_to_cache(e, rr);

        avahi_record_unref(rr);
    }

    /** Inform the user that this is the last reply */
    if (l->callback && !l->dead)
        l->callback(e, AVAHI_BROWSER_ALL_FOR_NOW, AVAHI_LOOKUP_RESULT_WIDE_AREA, NULL, l->userdata);
    
finish:
    if (l)
        lookup_stop(l);
}

static void socket_event(AvahiWatch *w, int fd, AvahiWatchEvent events, void *userdata) {
    AvahiWideAreaLookupEngine *e = userdata;
    AvahiAddress a;
    AvahiDnsPacket *p = NULL;
    
    if (fd == e->fd_ipv4) {
        struct sockaddr_in sa;
            
        if ((p = avahi_recv_dns_packet_ipv4(e->fd_ipv4, &sa, NULL, NULL, NULL)))
            avahi_address_from_sockaddr((struct sockaddr*) &sa, &a);
            
    } else if (fd == e->fd_ipv6) {
        struct sockaddr_in6 sa6;
        
        if ((p = avahi_recv_dns_packet_ipv6(e->fd_ipv6, &sa6, NULL, NULL, NULL)))
            avahi_address_from_sockaddr((struct sockaddr*) &sa6, &a);

    }

    if (p) {
        handle_packet(e, p, &a);
        avahi_dns_packet_free(p);
    }
}

AvahiWideAreaLookupEngine *avahi_wide_area_engine_new(AvahiServer *s) {
    AvahiWideAreaLookupEngine *e;
    
    assert(s);

    e = avahi_new(AvahiWideAreaLookupEngine, 1);
    e->server = s;
    e->cleanup_dead = 0;

    /* Create sockets */
    e->fd_ipv4 = avahi_open_unicast_socket_ipv4();
    e->fd_ipv6 = avahi_open_unicast_socket_ipv6();

    if (e->fd_ipv4 < 0 && e->fd_ipv6 < 0) {
        avahi_log_error(__FILE__": Failed to create wide area sockets: %s\n", strerror(errno));

        if (e->fd_ipv6 >= 0)
            close(e->fd_ipv6);

        if (e->fd_ipv4 >= 0)
            close(e->fd_ipv4);
        
        avahi_free(e);
        return NULL;
    }

    /* Create watches */
    if (e->fd_ipv4 >= 0)
        e->watch_ipv4 = s->poll_api->watch_new(e->server->poll_api, e->fd_ipv4, AVAHI_WATCH_IN, socket_event, e);
    if (e->fd_ipv6 >= 0)
        e->watch_ipv6 = s->poll_api->watch_new(e->server->poll_api, e->fd_ipv6, AVAHI_WATCH_IN, socket_event, e);

    e->n_dns_servers = e->current_dns_server = 0;
    e->next_id = (uint16_t) rand();

    /* Initialize cache */
    AVAHI_LLIST_HEAD_INIT(AvahiWideAreaCacheEntry, e->cache);
    e->cache_by_key = avahi_hashmap_new((AvahiHashFunc) avahi_key_hash, (AvahiEqualFunc) avahi_key_equal, (AvahiFreeFunc) avahi_key_unref, NULL);
    e->cache_n_entries = 0;

    /* Initialize lookup list */
    e->lookups_by_id = avahi_hashmap_new((AvahiHashFunc) avahi_int_hash, (AvahiEqualFunc) avahi_int_equal, NULL, NULL);
    e->lookups_by_key = avahi_hashmap_new((AvahiHashFunc) avahi_key_hash, (AvahiEqualFunc) avahi_key_equal, (AvahiFreeFunc) avahi_key_unref, NULL);
    AVAHI_LLIST_HEAD_INIT(AvahiWideAreaLookup, e->lookups);

    return e;
}

void avahi_wide_area_engine_free(AvahiWideAreaLookupEngine *e) {
    assert(e);
    
    avahi_wide_area_clear_cache(e);

    while (e->lookups)
        lookup_destroy(e->lookups);
    
    avahi_hashmap_free(e->cache_by_key);
    avahi_hashmap_free(e->lookups_by_id);
    avahi_hashmap_free(e->lookups_by_key);

    if (e->watch_ipv4)
        e->server->poll_api->watch_free(e->watch_ipv4);

    if (e->watch_ipv6)
        e->server->poll_api->watch_free(e->watch_ipv6);

    if (e->fd_ipv6 >= 0)
        close(e->fd_ipv6);
    
    if (e->fd_ipv4 >= 0)
        close(e->fd_ipv4);

    avahi_free(e);
}

void avahi_wide_area_clear_cache(AvahiWideAreaLookupEngine *e) {
    assert(e);

    while (e->cache)
        cache_entry_free(e->cache);

    assert(e->cache_n_entries == 0);
}


void avahi_wide_area_set_servers(AvahiWideAreaLookupEngine *e, const AvahiAddress *a, unsigned n) {
    assert(e);

    
    if (a) {
        for (e->n_dns_servers = 0; n > 0 && e->n_dns_servers < AVAHI_MAX_WIDE_AREA_SERVERS; a++, n--) 
            if ((a->proto == AVAHI_PROTO_INET && e->fd_ipv4 >= 0) || (a->proto == AVAHI_PROTO_INET6 && e->fd_ipv6 >= 0))
                e->dns_servers[e->n_dns_servers++] = *a;
    } else {
        assert(n == 0);
        e->n_dns_servers = 0;
    }
    
    e->current_dns_server = 0;

    avahi_wide_area_clear_cache(e);
}

void avahi_wide_area_cache_dump(AvahiWideAreaLookupEngine *e, AvahiDumpCallback callback, void* userdata) {
    AvahiWideAreaCacheEntry *c;
    
    assert(e);
    assert(callback);

    callback(";; WIDE AREA CACHE ;;; ", userdata);
    
    for (c = e->cache; c; c = c->cache_next) {
        char *t = avahi_record_to_string(c->record);
        callback(t, userdata);
        avahi_free(t);
    }
}

unsigned avahi_wide_area_scan_cache(AvahiWideAreaLookupEngine *e, AvahiKey *key, AvahiWideAreaLookupCallback callback, void *userdata) {
    AvahiWideAreaCacheEntry *c;
    AvahiKey *cname_key;
    unsigned n = 0;
    
    assert(e);
    assert(key);
    assert(callback);

    for (c = avahi_hashmap_lookup(e->cache_by_key, key); c; c = c->by_key_next) {
        callback(e, AVAHI_BROWSER_NEW, AVAHI_LOOKUP_RESULT_WIDE_AREA|AVAHI_LOOKUP_RESULT_CACHED, c->record, userdata);
        n++;
    }

    if ((cname_key = avahi_key_new_cname(key))) {

        for (c = avahi_hashmap_lookup(e->cache_by_key, cname_key); c; c = c->by_key_next) {
            callback(e, AVAHI_BROWSER_NEW, AVAHI_LOOKUP_RESULT_WIDE_AREA|AVAHI_LOOKUP_RESULT_CACHED, c->record, userdata);
            n++;
        }
        
        avahi_key_unref(cname_key);
    }

    return n;
}

int avahi_wide_area_has_servers(AvahiWideAreaLookupEngine *e) {
    assert(e);

    return e->n_dns_servers > 0;
}


    
