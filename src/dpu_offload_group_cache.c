//
// Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <limits.h>
#include <inttypes.h>

#include "dpu_offload_types.h"
#include "dpu_offload_debug.h"
#include "dpu_offload_group_cache.h"
#include "dpu_offload_event_channels.h"

extern dpu_offload_status_t send_sp_data_to_host(offloading_engine_t *engine, execution_context_t *econtext, ucp_ep_h host_ep, uint64_t host_dest_id);

// Forward declarations
static dpu_offload_status_t do_populate_group_cache_lookup_table(offloading_engine_t *engine, group_cache_t *gp_cache);
dpu_offload_status_t offload_engine_progress(offloading_engine_t *engine);
dpu_offload_status_t do_send_cache_entry_request(execution_context_t *econtext, ucp_ep_h ep, uint64_t dest_id, rank_info_t *requested_peer, dpu_offload_event_t *ev);
dpu_offload_status_t send_cache_entry_request(execution_context_t *econtext, ucp_ep_h ep, uint64_t dest_id, rank_info_t *requested_peer, dpu_offload_event_t **ev);

bool is_in_cache(cache_t *cache, group_uid_t gp_uid, int64_t rank_id, int64_t group_size)
{
    peer_cache_entry_t *entry = GET_GROUP_RANK_CACHE_ENTRY(cache, gp_uid, rank_id, group_size);
    if (entry == NULL)
        return false;
    return (entry->set);
}

bool group_cache_populated(offloading_engine_t *engine, group_uid_t gp_uid)
{
    group_cache_t *gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp_cache);
    if (gp_cache->revokes.global == 0 && gp_cache->group_size == gp_cache->num_local_entries)
    {
        DBG("Group cache for group 0x%x fully populated. num_local_entries = %" PRIu64 " group_size = %" PRIu64,
            gp_uid, gp_cache->num_local_entries, gp_cache->group_size);
        return true;
    }
    return false;
}

void display_group_cache(cache_t *cache, group_uid_t gp_uid)
{
    size_t i = 0;
    size_t idx = 0;
    group_cache_t *gp_cache = NULL;
    gp_cache = GET_GROUP_CACHE(cache, gp_uid);
    fprintf(stderr, "Content of cache for group 0x%x\n", gp_uid);
    fprintf(stderr, "-> group size: %ld\n", gp_cache->group_size);
    fprintf(stderr, "-> n_local_rank: %ld\n", gp_cache->n_local_ranks);
    fprintf(stderr, "-> n_local_ranks_populated: %ld\n", gp_cache->n_local_ranks_populated);
    fprintf(stderr, "-> num_local_entries: %ld\n", gp_cache->num_local_entries);
    fprintf(stderr, "-> sent_to_host (seq num): %ld\n\n", gp_cache->persistent.sent_to_host);

    while (i < gp_cache->group_size)
    {
        peer_cache_entry_t *entry = GET_GROUPRANK_CACHE_ENTRY(cache, gp_uid, idx);
        if (entry->set)
        {
            fprintf(stderr, "Rank %" PRId64 " host: 0x%lx\n", entry->peer.proc_info.group_rank, entry->peer.host_info);
            assert(idx == entry->peer.proc_info.group_rank);
            i++;
        }
        idx++;
    }
}


void group_cache_send_to_local_ranks_cb(void *context)
{
    group_cache_t *gp_cache = NULL;
    size_t new_revokes = 0;

    assert(context);
    gp_cache = (group_cache_t*) context;
    assert(gp_cache->engine);
    assert(gp_cache->revokes.global <= gp_cache->group_size);
    gp_cache->persistent.sent_to_host = gp_cache->persistent.num;

    DBG("Handling potential pending revoke messages (seq num: %ld, global revokes: %ld)",
        gp_cache->persistent.num, gp_cache->revokes.global);
    HANDLE_PENDING_GROUP_REVOKE_MSGS_FROM_SPS(gp_cache, new_revokes);
    assert(gp_cache->revokes.global <= gp_cache->group_size);
    DBG("%ld new revokes (seq num: %ld, revoke to ranks posted: %ld, global revokes: %ld)",
        new_revokes,
        gp_cache->persistent.num,
        gp_cache->persistent.revoke_send_to_host_posted,
        gp_cache->revokes.global);

    // If meanwhile the group has been revoked and the host not yet notified, we handle it since it is now safe to do so
    if (new_revokes > 0 &&
        gp_cache->persistent.revoke_send_to_host_posted < gp_cache->persistent.num &&
        gp_cache->revokes.global == gp_cache->group_size)
    {
        dpu_offload_status_t rc;
        offloading_engine_t *engine = (offloading_engine_t *)gp_cache->engine;
        assert(gp_cache->group_size);
        DBG("Sending revoke message to ranks for group 0x%x (size=%ld)", gp_cache->group_uid, gp_cache->group_size);
        rc = send_revoke_group_to_ranks(engine, gp_cache->group_uid, gp_cache->group_size);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("send_revoke_group_to_ranks() failed");
        }

    }
    assert(gp_cache->revokes.global <= gp_cache->group_size);
}

dpu_offload_status_t send_group_cache(execution_context_t *econtext, ucp_ep_h dest_ep, uint64_t dest_id, group_uid_t gp_uid, dpu_offload_event_t *metaev)
{
    size_t i;
    int rc;
    group_cache_t *gp_cache;
    assert(econtext);
    assert(econtext->engine);
    assert(metaev);
    assert(EVENT_HDR_TYPE(metaev) == META_EVENT_TYPE);
    gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), gp_uid);
    assert(gp_cache);
    assert(gp_cache->engine);
    if (!gp_cache->initialized)
        return DO_SUCCESS;

    assert(gp_cache->group_size > 0);

    // The entire group is supposed to be ready, starting at rank 0
#if !NDEBUG
    for (i = 0; i < gp_cache->group_size; i++)
    {
        peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(econtext->engine->procs_cache), gp_uid, i, gp_cache->group_size);
        assert(cache_entry->set == true);
        assert(cache_entry->num_shadow_service_procs > 0);
        assert(cache_entry->peer.proc_info.group_seq_num);
    }
#endif

    dpu_offload_event_t *e;
    peer_cache_entry_t *first_entry = GET_GROUP_RANK_CACHE_ENTRY(&(econtext->engine->procs_cache), gp_uid, 0, gp_cache->group_size);
    rc = event_get(econtext->event_channels, NULL, &e);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
    e->is_subevent = true;
    DBG("Sending %ld cache entries to %ld, ev: %p (%ld), metaev: %ld\n",
        gp_cache->group_size, dest_id, e, e->seq_num, metaev->seq_num);
    rc = event_channel_emit_with_payload(&e, AM_PEER_CACHE_ENTRIES_MSG_ID, dest_ep, dest_id, NULL, first_entry, gp_cache->group_size * sizeof(peer_cache_entry_t));
    if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
    {
        ERR_MSG("event_channel_emit_with_payload() failed");
        return DO_ERROR;
    }
    if (e != NULL)
    {
        QUEUE_SUBEVENT(metaev, e);
    }
    else
    {
        WARN_MSG("Sending cache completed right away");
    }
    return DO_SUCCESS;
}

dpu_offload_status_t send_gp_cache_to_host(execution_context_t *econtext, group_uid_t group_uid)
{
    assert(econtext->type == CONTEXT_SERVER);
    assert(econtext->scope_id == SCOPE_HOST_DPU);
    size_t n = 0, idx = 0;
    dpu_offload_status_t rc;
    group_cache_t *gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), group_uid);
    assert(gp_cache);
    assert(gp_cache->engine);
    assert(gp_cache->n_sps);
    if (gp_cache->persistent.sent_to_host < gp_cache->persistent.num)
    {
        dpu_offload_event_t *metaev;

        DBG("Cache is complete for group 0x%x (seq_num: %ld), sending it to the local ranks (econtext: %p, number of connected clients: %ld, total: %ld)",
            group_uid,
            gp_cache->persistent.num,
            econtext,
            econtext->server->connected_clients.num_connected_clients,
            econtext->server->connected_clients.num_total_connected_clients);
        assert(group_cache_populated(econtext->engine, group_uid));
        assert(gp_cache->group_uid == group_uid);

        rc = event_get(econtext->event_channels, NULL, &metaev);
        CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
        assert(metaev);
        assert(metaev->ctx.completion_cb == NULL);
        EVENT_HDR_TYPE(metaev) = META_EVENT_TYPE;
        metaev->ctx.completion_cb = group_cache_send_to_local_ranks_cb;
        metaev->ctx.completion_cb_ctx = gp_cache;

        while (n < econtext->server->connected_clients.num_connected_clients)
        {
            peer_info_t *c = DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients),
                                               idx, peer_info_t);
            if (c == NULL)
            {
                idx++;
                continue;
            }

            if (gp_cache->group_uid == econtext->engine->procs_cache.world_group)
            {
                // If we are dealing with the world group, we know now that we know
                // about all the SPs involved in the job so we send the SPs' data
                // to the local ranks so we can propagate the data as soon as it is
                // all available. Note that at bootstrapping time, hosts only know
                // about their associated SPs, not all SPs.
                rc = send_sp_data_to_host(econtext->engine, econtext, c->ep, c->id);
                CHECK_ERR_RETURN((rc), DO_ERROR, "sendd_sp_data_to_host() failed");
            }

            DBG("Send cache to client #%ld (id: %" PRIu64 ")", idx, c->id);
            rc = send_group_cache(econtext, c->ep, c->id, group_uid, metaev);
            CHECK_ERR_RETURN((rc), DO_ERROR, "send_group_cache() failed");
            n++;
            idx++;
        }

        // Once the cache is sent to the host, we know it cannot change so we
        // populate the few lookup table.
        // Note that we pre-emptively create the cache on the SPs, it might not
        // be the case on the host where these tables may be populated in a lazy
        // manner.
        rc = populate_group_cache_lookup_table(econtext->engine, gp_cache);
        CHECK_ERR_RETURN((rc), DO_ERROR, "populate_group_cache_lookup_table() failed");

        // We check for completion only after populating the topology because in some
        // corner cases (e.g., the SP not being involved in the group at all), completion
        // may lead to the group being revoked.
        if (!event_completed(metaev))
            QUEUE_EVENT(metaev);
        else
            event_return(&metaev);
    }
    else
        DBG("cache aleady sent to host");
    return DO_SUCCESS;
}

dpu_offload_status_t handle_peer_cache_entries_recv(execution_context_t *econtext, uint64_t sp_gid, void *data, size_t data_len)
{
    size_t cur_size = 0;
    size_t idx = 0;
    int64_t group_rank, group_size;
    size_t n_added = 0;
    group_cache_t *gp_cache = NULL;
    peer_cache_entry_t *entries = (peer_cache_entry_t *)data;
    cache_t *cache = NULL;
    offloading_engine_t *engine = NULL;
    int group_uid = INT_MAX;

    assert(econtext);
    engine = econtext->engine;
    assert(engine);
    cache = &(engine->procs_cache);
    group_size = entries[idx].peer.proc_info.group_size;
    group_uid = entries[idx].peer.proc_info.group_uid;
    gp_cache = GET_GROUP_CACHE(&(econtext->engine->procs_cache), group_uid);
    assert(gp_cache);

    while (cur_size < data_len)
    {
        // Now that we know for sure we have the group ID, we can move the received data into the local cache
        group_rank = entries[idx].peer.proc_info.group_rank;
#if !NDEBUG
        if (entries[idx].peer.proc_info.group_uid != group_uid)
        {
            ERR_MSG("Invalid group ID: %d vs. %d", entries[idx].peer.proc_info.group_uid, group_uid);
            return DO_ERROR;
        }
#endif
        assert(entries[idx].peer.proc_info.group_size == group_size);
        DBG("Received a cache entry for rank:%ld, group:0x%x, group size:%ld, group seq num: %ld, number of local rank: %ld from SP %" PRId64 " (msg size=%ld, peer addr len=%ld)",
            group_rank,
            group_uid,
            group_size,
            entries[idx].peer.proc_info.group_seq_num,
            entries[idx].peer.proc_info.n_local_ranks,
            sp_gid,
            data_len,
            entries[idx].peer.addr_len);
        if (!is_in_cache(cache, group_uid, group_rank, group_size))
        {
            peer_cache_entry_t *cache_entry = NULL;
            size_t n;

            // Make sure the entry is for the "version" of the group that matches
            assert(entries[idx].peer.proc_info.group_seq_num);
            if (gp_cache->num_local_entries == 0)
            {
                // New "version" of the group.
                assert(gp_cache->persistent.sent_to_host == gp_cache->persistent.num);
                gp_cache->persistent.num++;
                DBG("Switched to seq num: %ld for group 0x%x", gp_cache->persistent.num, gp_cache->group_uid);
            }
            assert(entries[idx].peer.proc_info.group_seq_num == gp_cache->persistent.num);

            if (gp_cache->group_uid == INT_MAX)
                gp_cache->group_uid = group_uid;
            n_added++;
            gp_cache->num_local_entries++;
            DBG("Adding rank %ld to group 0x%x (seq_num: %ld/%ld)",
                group_rank, gp_cache->group_uid, gp_cache->persistent.num, entries[idx].peer.proc_info.group_seq_num);
            cache_entry = GET_GROUP_RANK_CACHE_ENTRY(cache, group_uid, group_rank, group_size);
            cache_entry->set = true;
            COPY_PEER_DATA(&(entries[idx].peer), &(cache_entry->peer));
            assert(entries[idx].num_shadow_service_procs > 0);
            assert(cache_entry->peer.proc_info.group_seq_num);
            // append the shadow DPU data to the data already local available (if any)
            for (n = 0; n < entries[idx].num_shadow_service_procs; n++)
            {
                dpu_offload_status_t rc;
                cache_entry->shadow_service_procs[cache_entry->num_shadow_service_procs + n] = entries[idx].shadow_service_procs[n];
                rc = update_topology_data(engine,
                                          gp_cache,
                                          group_rank,
                                          entries[idx].shadow_service_procs[n],
                                          entries[idx].peer.host_info);
                CHECK_ERR_RETURN((rc), DO_ERROR, "handle_cache_data() failed");
            }
            cache_entry->num_shadow_service_procs += entries[idx].num_shadow_service_procs;
            cache_entry->client_id = entries[idx].client_id;

            // If any event is associated to the cache entry, handle them
            if (cache_entry->events_initialized)
            {
                while (!SIMPLE_LIST_IS_EMPTY(&(cache_entry->events)))
                {
                    dpu_offload_event_t *e = SIMPLE_LIST_EXTRACT_HEAD(&(cache_entry->events), dpu_offload_event_t, item);
                    COMPLETE_EVENT(e);
                    event_return(&e);
                }
            }

            DBG("Cache now has %ld local entries and group size is %ld", gp_cache->num_local_entries, gp_cache->group_size);

#if !NDEBUG
            if (gp_cache->num_local_entries == gp_cache->group_size)
                DBG("Group cache is now complete");
#endif
        }
        cur_size += sizeof(peer_cache_entry_t);
        idx++;
    }

    // Once we handled all the cache entries we received, we check whether the cache is full and if so, send it to the local ranks
    DBG("The cache for group 0x%x now has %ld entries after receiving data from SP %" PRIu64 " (group size: %ld)",
        group_uid, gp_cache->num_local_entries, sp_gid, gp_cache->group_size);
    if (econtext->engine->on_dpu && n_added > 0)
    {
        // If all the ranks are on the local hosts, the case is handled in the callback that deals with the
        // final step of the connecting with the ranks.
        bool all_ranks_are_local = false;
        if (econtext->engine->config->num_service_procs_per_dpu == 1 && gp_cache->group_size == gp_cache->n_local_ranks)
            all_ranks_are_local = true;
        if (gp_cache->group_size > 0 && gp_cache->num_local_entries == gp_cache->group_size && !all_ranks_are_local)
        {
            DBG("Sending group cache for group 0x%x to local ranks (gp_sz=%ld)", gp_cache->group_uid, gp_cache->group_size);
            execution_context_t *server = get_server_servicing_host(engine);
            assert(server->scope_id == SCOPE_HOST_DPU);
            dpu_offload_status_t rc = send_gp_cache_to_host(server, group_uid);
            CHECK_ERR_RETURN((rc), DO_ERROR, "send_gp_cache_to_host() failed");
        }
        else
        {
            DBG("Cache 0x%x (%d) is still missing some data. group_size: %ld, num_local_entries: %ld",
                gp_cache->group_uid, gp_cache->group_uid, gp_cache->group_size, gp_cache->num_local_entries);
        }
    }
    return DO_SUCCESS;
}


/**
 * @brief Actually revoke a group: all the elements in the rank array are reset and the cache itself is also
 * reset.
 * Can do used both on the hosts and DPUs.
 *
 * @param[in] engine Associated offloading engine
 * @param[in] gp_uid Unique ID of the group being revoked
 * @return dpu_offload_status_t
 */
dpu_offload_status_t revoke_group_cache(offloading_engine_t *engine, group_uid_t gp_uid)
{
    size_t i;
    notification_callback_entry_t *cb = NULL;
    group_cache_t *c = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(c);

#if !NDEBUG
    if (engine->on_dpu)
    {
        assert(c->persistent.sent_to_host == c->persistent.num);
        assert(c->persistent.revoke_sent_to_host == c->persistent.num);
    }
#endif
    DBG("Revoking group 0x%x (seq num: %ld)", gp_uid, c->persistent.num);
    assert(c->group_size);
    for (i = 0; i < c->group_size; i++)
    {
        peer_cache_entry_t *e = DYN_ARRAY_GET_ELT(&(c->ranks),
                                                  i,
                                                  peer_cache_entry_t);
        assert(e);
        RESET_PEER_CACHE_ENTRY(e);
    }
    if (c->sps_bitset != NULL)
    {
        GROUP_CACHE_BITSET_DESTROY(c->sps_bitset);
    }
    if (c->hosts_bitset != NULL)
    {
        GROUP_CACHE_BITSET_DESTROY(c->hosts_bitset);
    }
    RESET_GROUP_CACHE(engine, c);
    assert(c->revokes.local == 0);
    assert(c->revokes.global == 0);

    // We invoke the handler for the internal MIMOSA group revoke event when one is registered
    cb = get_notif_callback_entry(engine->self_econtext->event_channels, MIMOSA_GROUP_REVOKE_EVENT_ID);
    if (cb != NULL && cb->cb != NULL)
    {
        int rc;
        assert(engine->self_econtext);
        rc = cb->cb(engine->self_econtext->event_channels, engine->self_econtext, NULL, 0, c, sizeof(group_cache_t));
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("callback for event of type %d failed (rc: %d)", MIMOSA_GROUP_REVOKE_EVENT_ID, rc);
        }
    }

    // Handle potential pending receives of cache entries
    HANDLE_PENDING_CACHE_ENTRIES(c);

    return DO_SUCCESS;
}

dpu_offload_status_t
get_global_sp_id_by_group(offloading_engine_t *engine,
                          group_uid_t gp_uid,
                          uint64_t *sp_id)
{
    size_t i;
    group_cache_t *gp_cache = NULL;

    assert(engine);
    if (!engine->on_dpu)
        return DO_ERROR;

    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp_cache);
    for (i = 0; i < gp_cache->n_sps; i++)
    {
        remote_service_proc_info_t **ptr = NULL;

        ptr = DYN_ARRAY_GET_ELT(&(gp_cache->sps),
                                i,
                                remote_service_proc_info_t *);
        assert(ptr);
        if ((*ptr)->service_proc.global_id == engine->config->local_service_proc.info.global_id)
        {
            *sp_id = engine->config->local_service_proc.info.global_id;
            return DO_SUCCESS;
        }
    }
    // The SP is not in the group, which is unexpected so an error
    return DO_ERROR;
}

dpu_offload_status_t
get_local_sp_id_by_group(offloading_engine_t *engine,
                         group_uid_t gp_uid,
                         uint64_t sp_gp_guid,
                         uint64_t *sp_gp_lid)
{
    remote_service_proc_info_t **ptr = NULL;
    sp_cache_data_t *sp_data = NULL;
    group_cache_t *gp_cache = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    ptr = DYN_ARRAY_GET_ELT(&(gp_cache->sps), sp_gp_guid, remote_service_proc_info_t *);
    assert(ptr);
    sp_data = GET_GROUP_SP_HASH_ENTRY(gp_cache, (*ptr)->service_proc.global_id);
    assert(sp_data);
    *sp_gp_lid = sp_data->lid;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_host_idx_by_group(offloading_engine_t *engine,
                      group_uid_t group_uid,
                      size_t *host_idx)
{
    group_cache_t *gp_cache = NULL;
    host_uid_t my_host_uid;
    size_t i;

    assert(engine);
    my_host_uid = engine->config->local_service_proc.host_uid;
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    for (i = 0; i < gp_cache->n_hosts; i++)
    {
        host_info_t **ptr = NULL;
        ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                                i,
                                host_info_t *);
        assert(ptr);
        if ((*ptr)->uid == my_host_uid)
        {
            *host_idx = i;
            return DO_SUCCESS;
        }
    }
    // The host is not in the group, which is not expected so an error.
    return DO_ERROR;
}

dpu_offload_status_t
get_num_sps_by_group_host_idx(offloading_engine_t *engine,
                              group_uid_t group_uid,
                              size_t host_idx,
                              size_t *num_sps)
{
    host_info_t **ptr = NULL;
    host_cache_data_t *host_data = NULL;
    group_cache_t *gp_cache = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                            host_idx,
                            host_info_t *);
    assert(ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*ptr)->uid);
    assert(host_data);
    *num_sps = host_data->num_sps;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_sp(offloading_engine_t *engine,
                           group_uid_t group_uid,
                           uint64_t sp_gp_gid,
                           size_t *num_ranks)
{
    remote_service_proc_info_t **sp_data = NULL;
    sp_cache_data_t *sp_info = NULL;
    group_cache_t *gp_cache = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    sp_data = DYN_ARRAY_GET_ELT(&(gp_cache->sps), sp_gp_gid, remote_service_proc_info_t *);
    assert(sp_data);
    sp_info = GET_GROUP_SP_HASH_ENTRY(gp_cache, (*sp_data)->service_proc.global_id);
    assert(sp_info);
    *num_ranks = sp_info->n_ranks;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_host_local_sp(offloading_engine_t *engine,
                                      group_uid_t group_uid,
                                      size_t host_idx,
                                      uint64_t local_host_sp_id,
                                      size_t *num_ranks)
{
    group_cache_t *gp_cache = NULL;
    host_info_t **host_info_ptr = NULL;
    host_cache_data_t *host_data = NULL;
    sp_cache_data_t **sp_data_ptr = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    host_info_ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts), host_idx, host_info_t *);
    assert(host_info_ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*host_info_ptr)->uid);
    assert(host_data);
    if (local_host_sp_id >= host_data->num_sps)
    {
        // The requested local SP is beyond the number of SP associated to the host
        // and involved in the group. This is an error.
        return DO_ERROR;
    }
    sp_data_ptr = DYN_ARRAY_GET_ELT(&(host_data->sps), local_host_sp_id, sp_cache_data_t *);
    assert(sp_data_ptr);
    *num_ranks = (*sp_data_ptr)->n_ranks;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_num_ranks_for_group_host_idx(offloading_engine_t *engine,
                                 group_uid_t group_uid,
                                 size_t host_idx,
                                 size_t *num_ranks)
{
    host_info_t **ptr = NULL;
    group_cache_t *gp_cache = NULL;
    host_cache_data_t *host_data = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                            host_idx,
                            host_info_t *);
    assert(ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*ptr)->uid);
    assert(host_data);
    *num_ranks = host_data->num_ranks;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_rank_idx_by_group_host_idx(offloading_engine_t *engine,
                               group_uid_t group_uid,
                               size_t host_idx,
                               int64_t rank,
                               uint64_t *idx)
{
    group_cache_t *gp_cache = NULL;
    host_info_t **host_info_ptr = NULL;
    host_cache_data_t *host_data = NULL;
    size_t i, rank_index = 0;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    host_info_ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts), host_idx, host_info_t *);
    assert(host_info_ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*host_info_ptr)->uid);
    assert(host_data);
    if (!GROUP_CACHE_BITSET_TEST(host_data->ranks_bitset, rank))
    {
        // The rank is not involved in the group and running on that host, error
        return DO_ERROR;
    }

    // From there we know the rank is on the host and involved in the rank, we just need
    // to find its index
    for (i = 0; i < host_data->num_ranks; i++)
    {
        if (i == rank)
            break;

        if (GROUP_CACHE_BITSET_TEST(host_data->ranks_bitset, i))
            rank_index++;
    }
    *idx = rank_index;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_rank_idx_by_group_sp_id(offloading_engine_t *engine,
                            group_uid_t group_uid,
                            uint64_t sp_gp_gid,
                            int64_t rank,
                            size_t *rank_idx)
{
    remote_service_proc_info_t **sp_data = NULL;
    sp_cache_data_t *sp_info = NULL;
    group_cache_t *gp_cache = NULL;
    size_t rank_index;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    sp_data = DYN_ARRAY_GET_ELT(&(gp_cache->sps), sp_gp_gid, remote_service_proc_info_t *);
    assert(sp_data);
    sp_info = GET_GROUP_SP_HASH_ENTRY(gp_cache, (*sp_data)->service_proc.global_id);
    assert(sp_info);

    for (rank_index = 0; rank_index < sp_info->n_ranks; rank_index++)
    {
        peer_cache_entry_t **rank_info = NULL;
        rank_info = DYN_ARRAY_GET_ELT(&(sp_info->ranks), rank_index, peer_cache_entry_t *);
        assert(rank_info);
        if ((*rank_info)->peer.proc_info.group_rank == rank)
        {
            *rank_idx = rank_index;
            return DO_SUCCESS;
        }
    }

    // We did not find the rank
    *rank_idx = UINT32_MAX;
    return DO_ERROR;
}

dpu_offload_status_t
get_all_sps_by_group_host_idx(offloading_engine_t *engine,
                              group_uid_t group_uid,
                              size_t host_idx,
                              dyn_array_t **sps,
                              size_t *num_sps)
{
    group_cache_t *gp_cache = NULL;
    host_info_t **host_info_ptr = NULL;
    host_cache_data_t *host_data = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    host_info_ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts), host_idx, host_info_t *);
    assert(host_info_ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*host_info_ptr)->uid);
    assert(host_data);
    *sps = &(host_data->sps);
    *num_sps = host_data->num_sps;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_hosts_by_group(offloading_engine_t *engine,
                       group_uid_t group_uid,
                       dyn_array_t **hosts,
                       size_t *num_hosts)
{
    group_cache_t *gp_cache = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    *hosts = &(gp_cache->hosts);
    *num_hosts = gp_cache->n_hosts;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_ranks_by_group_sp_gid(offloading_engine_t *engine,
                              group_uid_t group_uid,
                              uint64_t sp_group_gid,
                              dyn_array_t **ranks,
                              size_t *num_ranks)
{
    group_cache_t *gp_cache = NULL;
    remote_service_proc_info_t **ptr = NULL;
    sp_cache_data_t *sp_data = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    ptr = DYN_ARRAY_GET_ELT(&(gp_cache->sps),
                            sp_group_gid,
                            remote_service_proc_info_t *);
    assert(ptr);
    sp_data = GET_GROUP_SP_HASH_ENTRY(gp_cache, (*ptr)->service_proc.global_id);
    assert(sp_data);
    *ranks = &(sp_data->ranks);
    *num_ranks = sp_data->n_ranks;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_all_ranks_by_group_sp_lid(offloading_engine_t *engine,
                              group_uid_t group_uid,
                              size_t host_idx,
                              uint64_t sp_group_lid,
                              dyn_array_t **ranks,
                              size_t *num_ranks)
{
    group_cache_t *gp_cache = NULL;
    host_info_t **host_info_ptr = NULL;
    host_cache_data_t *host_data = NULL;
    sp_cache_data_t **sp_data_ptr = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    // Get the host data
    host_info_ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts), host_idx, host_info_t *);
    assert(host_info_ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*host_info_ptr)->uid);
    assert(host_data);

    // Get the SP's data
    sp_data_ptr = DYN_ARRAY_GET_ELT(&(host_data->sps), sp_group_lid, sp_cache_data_t *);
    assert(sp_data_ptr);
    *ranks = &((*sp_data_ptr)->ranks);
    *num_ranks = (*sp_data_ptr)->n_ranks;
    return DO_SUCCESS;
}

dpu_offload_status_t
get_nth_sp_by_group_host_idx(offloading_engine_t *engine,
                             group_uid_t group_uid,
                             size_t host_idx,
                             size_t n,
                             uint64_t *global_group_sp_id)
{
    group_cache_t *gp_cache = NULL;
    host_info_t **host_ptr = NULL;
    host_cache_data_t *host_data = NULL;
    sp_cache_data_t **sp_data_ptr = NULL;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    // Lookup the host's data
    host_ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                                 host_idx,
                                 host_info_t *);
    assert(host_ptr);
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, (*host_ptr)->uid);
    if (n >= host_data->num_sps)
        return DO_ERROR;

    // Lookup the SP's data
    sp_data_ptr = DYN_ARRAY_GET_ELT(&(host_data->sps), n, sp_cache_data_t *);
    assert(sp_data_ptr);
    *global_group_sp_id = (*sp_data_ptr)->gid;
    return DO_SUCCESS;
}

dpu_offload_status_t get_sp_group_gid(offloading_engine_t *engine,
                                      group_uid_t group_uid,
                                      uint64_t sp_gid,
                                      uint64_t *sp_gp_gid)
{
    group_cache_t *gp_cache = NULL;
    size_t sp_gp_idx;

    assert(engine);
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), group_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

#if !NDEBUG
    if (!gp_cache->sp_array_initialized && !group_cache_populated(engine, group_uid))
    {
        ERR_MSG("Group cache lookup tables are not created and the group cache incomplete");
        return DO_ERROR;
    }
#endif

    assert(gp_cache->sp_array_initialized);
    for (sp_gp_idx = 0; sp_gp_idx < gp_cache->n_sps; sp_gp_idx++)
    {
        remote_service_proc_info_t **sp_data = NULL;
        sp_data = DYN_ARRAY_GET_ELT(&(gp_cache->sps), sp_gp_idx, remote_service_proc_info_t *);
        assert(sp_data);

        if ((*sp_data)->service_proc.global_id == sp_gid)
        {
            *sp_gp_gid = sp_gp_idx;
            return DO_SUCCESS;
        }
    }
    *sp_gp_gid = UINT64_MAX;
    return DO_ERROR;
}

dpu_offload_status_t get_group_ranks_on_host(offloading_engine_t *engine,
                                             group_uid_t gp_uid,
                                             uint64_t host_id,
                                             size_t *n_ranks,
                                             dyn_array_t *ranks)
{
    group_cache_t *gp = NULL;
    int64_t i, num = 0;

    *n_ranks = 0;
    assert(engine);
    gp = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp);

    if (!gp->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    for (i = 0; i < gp->group_size; i++)
    {
        peer_cache_entry_t *peer;
        peer = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache), gp_uid, i, GROUP_SIZE_UNKNOWN);
        if (peer->peer.host_info == host_id)
        {
            int64_t *rank_entry = NULL;
            rank_entry = DYN_ARRAY_GET_ELT(ranks, num, int64_t);
            assert(rank_entry);
            *rank_entry = i;
            num++;
        }
    }
    *n_ranks = num;
    return DO_SUCCESS;
}

dpu_offload_status_t get_group_local_sps(offloading_engine_t *engine,
                                         group_uid_t gp_uid,
                                         size_t *n_sps,
                                         dyn_array_t *sps)
{
    group_cache_t *gp = NULL;
    size_t i, num = 0;

    assert(engine);
    *n_sps = 0;
    if (!engine->on_dpu)
        return DO_SUCCESS;
    assert(engine->host_id != UINT64_MAX);
    gp = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp);

    if (!gp->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    for (i = 0; i < gp->group_size; i++)
    {
        peer_cache_entry_t *peer;
        peer = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache), gp_uid, i, GROUP_SIZE_UNKNOWN);
        if (peer->peer.host_info == engine->host_id)
        {
            size_t sp_idx;
            for (sp_idx = 0; sp_idx < peer->num_shadow_service_procs; sp_idx++)
            {
                uint64_t *sp_id_entry = NULL;
                sp_id_entry = DYN_ARRAY_GET_ELT(sps, num, uint64_t);
                assert(sp_id_entry);
                *sp_id_entry = peer->shadow_service_procs[sp_idx];
                num++;
            }
        }
    }
    *n_sps = num;
    return DO_SUCCESS;
}

dpu_offload_status_t get_group_rank_host(offloading_engine_t *engine,
                                         group_uid_t gp_uid,
                                         int64_t rank,
                                         uint64_t *host_id)
{
    *host_id = UINT64_MAX;
    assert(engine);
    if (is_in_cache(&(engine->procs_cache), gp_uid, rank, -1))
    {
        // The cache has the data
        peer_cache_entry_t *cache_entry = NULL;
        cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache), gp_uid, rank, GROUP_SIZE_UNKNOWN);
        assert(cache_entry);
        *host_id = cache_entry->peer.host_info;
        return DO_SUCCESS;
    }

    return DO_ERROR;
}

dpu_offload_status_t get_group_rank_sps(offloading_engine_t *engine,
                                        group_uid_t gp_uid,
                                        uint64_t rank,
                                        size_t *n_sps,
                                        dyn_array_t **sps)
{
    dpu_offload_status_t rc;
    uint64_t host_id;
    group_cache_t *gp = NULL;

    gp = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp);

    if (!gp->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
            return rc;
        }
    }

    *n_sps = 0;
    rc = get_group_rank_host(engine, gp_uid, rank, &host_id);
    CHECK_ERR_RETURN((rc), DO_ERROR, "get_group_rank_host() failed");


    host_cache_data_t *host_info = NULL;
    host_info = GET_GROUP_HOST_HASH_ENTRY(gp, host_id);
    assert(host_info);
    *n_sps = host_info->num_sps;
    *sps = &(host_info->sps);
    return DO_SUCCESS;
}

static void
populate_sp_ranks(offloading_engine_t *engine, group_cache_t *gp_cache, sp_cache_data_t *sp_data)
{
    size_t i = 0, idx = 0;
    DYN_ARRAY_ALLOC(&(sp_data->ranks),
                    gp_cache->group_size,
                    peer_cache_entry_t *);
    sp_data->ranks_initialized = true;
    assert(sp_data->n_ranks);
    while (idx < sp_data->n_ranks)
    {
        if (GROUP_CACHE_BITSET_TEST(sp_data->ranks_bitset, i))
        {
            peer_cache_entry_t *rank_info = NULL;
            peer_cache_entry_t **ptr = NULL;
            rank_info = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache),
                                                   gp_cache->group_uid,
                                                   i,
                                                   gp_cache->group_size);
            assert(rank_info);
            ptr = DYN_ARRAY_GET_ELT(&(sp_data->ranks), idx, peer_cache_entry_t *);
            assert(ptr);
            (*ptr) = rank_info;
            idx++;
        }
        i++;
    }
    assert(idx == sp_data->n_ranks);
}

static void
populate_host_sps(group_cache_t *gp_cache, host_cache_data_t *host_data)
{
    size_t i = 0, idx = 0;
    DYN_ARRAY_ALLOC(&(host_data->sps),
                    gp_cache->group_size,
                    sp_cache_data_t *);
    host_data->sps_initialized = true;
    while (idx < host_data->num_sps)
    {
        if (GROUP_CACHE_BITSET_TEST(host_data->sps_bitset, i))
        {
            sp_cache_data_t **ptr = NULL, *sp_info = NULL;
            sp_info = GET_GROUP_SP_HASH_ENTRY(gp_cache, i);
            assert(sp_info);
            ptr = DYN_ARRAY_GET_ELT(&(host_data->sps), idx, sp_cache_data_t *);
            assert(ptr);
            sp_info->lid = idx;
            (*ptr) = sp_info;
            idx++;
        }
        i++;
    }
    assert(idx == host_data->num_sps);
}

static dpu_offload_status_t
do_populate_group_cache_lookup_table(offloading_engine_t *engine, group_cache_t *gp_cache)
{
    size_t i, idx = 0;

    assert(engine);
    assert(gp_cache);

    if (gp_cache->lookup_tables_populated)
        return DO_SUCCESS;

    DBG("Creating the contiguous and ordered list of SPs involved in the group");
    assert(gp_cache->n_sps);
    if (gp_cache->sp_array_initialized == false)
    {
        DYN_ARRAY_ALLOC(&(gp_cache->sps),
                        gp_cache->n_sps,
                        remote_service_proc_info_t *);
        gp_cache->sp_array_initialized = true;
    }

    i = 0;
    while (i < gp_cache->n_sps)
    {
        if (GROUP_CACHE_BITSET_TEST(gp_cache->sps_bitset, idx))
        {
            remote_service_proc_info_t *sp_data = NULL, **ptr = NULL;
            sp_data = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(engine),
                                        idx,
                                        remote_service_proc_info_t);
            assert(sp_data);
            ptr = DYN_ARRAY_GET_ELT(&(gp_cache->sps),
                                    i,
                                    remote_service_proc_info_t *);
            *ptr = sp_data;
            i++;
        }
        idx++;
    }

    DBG("Creating the contiguous and ordered list of ranks associated with each SP");
    assert(kh_size(gp_cache->sps_hash) == gp_cache->n_sps);
    if (kh_size(gp_cache->sps_hash) != 0)
    {
        uint64_t sp_key;
        sp_cache_data_t *sp_value = NULL;
        kh_foreach(gp_cache->sps_hash, sp_key, sp_value, {
            populate_sp_ranks(engine, gp_cache, sp_value);
        })
    }

    DBG("Creating the contiguous and ordered list of hosts involved in the group");
    if (gp_cache->host_array_initialized == false)
    {
        DYN_ARRAY_ALLOC(&(gp_cache->hosts),
                        gp_cache->n_hosts,
                        host_info_t *);
        gp_cache->host_array_initialized = true;
    }
    i = 0;
    idx = 0;
    while (i < gp_cache->n_hosts)
    {
        if (GROUP_CACHE_BITSET_TEST(gp_cache->hosts_bitset, idx))
        {
            host_info_t *info = NULL, **ptr = NULL;
            info = DYN_ARRAY_GET_ELT(&(engine->config->hosts_config),
                                     idx,
                                     host_info_t);
            assert(info);
            ptr = DYN_ARRAY_GET_ELT(&(gp_cache->hosts),
                                    i,
                                    host_info_t *);
            *ptr = info;
            i++;
        }
        idx++;
    }

    DBG("Handling data of SPs in the context of hosts");
    if (kh_size(gp_cache->hosts_hash) != 0)
    {
        uint64_t host_key;
        host_cache_data_t *host_value = NULL;
        kh_foreach(gp_cache->hosts_hash, host_key, host_value, {
            populate_host_sps(gp_cache, host_value);
        })
    }

    gp_cache->lookup_tables_populated = true;
    return DO_SUCCESS;
}

dpu_offload_status_t
populate_group_cache_lookup_table(offloading_engine_t *engine,
                                  group_cache_t *gp_cache)
{
    assert(gp_cache);
    assert(group_cache_populated(engine, gp_cache->group_uid));
    return do_populate_group_cache_lookup_table(engine, gp_cache);
}

dpu_offload_status_t
update_topology_data(offloading_engine_t *engine, group_cache_t *gp_cache, int64_t group_rank, uint64_t sp_gid, host_uid_t host_uid)
{

    sp_cache_data_t *sp_data = NULL;
    host_cache_data_t *host_data = NULL;

    assert(engine);
    assert(gp_cache);

    // SPs have a unique ID, are all known, as well as the host associated to them.
    // So when we receive a cache entry, we look the SP of the cache entry and update
    // a SP lookup table so we can track which SPs are involved in the group.
    // As mentioned, knowing which SPs are involed in the group also allows us to track
    // which hosts are involved in the group. Note that it is difficult to directly
    // track which hosts are involved because host are represented via a hash and
    // it is therefore difficult to keep an ordered list of hosts that is consistent
    // everywhere.

    // Check if the SP is already in the group SP hash; if not, it means it is first
    // time we learn about that SP in the group so we increment the number of SPs
    // involved in the group
    sp_data = GET_GROUP_SP_HASH_ENTRY(gp_cache, sp_gid);
    if (sp_data == NULL)
    {
        // SP is not in the hash, we start by updating some bookkeeping variables
        DBG("group cache does not have SP %" PRIu64 ", adding SP to hash for the group (0x%x)",
            sp_gid, gp_cache->group_uid);
        gp_cache->n_sps++;
        // Add the SP to the hash using the global SP id as key
        DYN_LIST_GET(engine->free_sp_cache_hash_obj,
                     sp_cache_data_t,
                     item,
                     sp_data);
        RESET_SP_CACHE_DATA(sp_data);
        GROUP_CACHE_BITSET_CREATE(sp_data->ranks_bitset, gp_cache->group_size);
        sp_data->gid = sp_gid;
        sp_data->n_ranks = 1;
        sp_data->gp_uid = gp_cache->group_uid;
        sp_data->host_uid = host_uid;
        // If the sps bitset is not initialized, initialize it right now
        GROUP_CACHE_BITSET_CREATE(gp_cache->sps_bitset, gp_cache->group_size);
        ADD_GROUP_SP_HASH_ENTRY(gp_cache, sp_data);
        GROUP_CACHE_BITSET_SET(gp_cache->sps_bitset, sp_gid);
    }
    else
    {
        // The SP is already in the hash
        sp_data->n_ranks++;
        DBG("cache entry has SP %" PRIu64 ", updating SP hash for the group (0x%x), # of ranks = %ld",
            sp_gid, gp_cache->group_uid, sp_data->n_ranks);
    }
    // Make the rank as associated to the SP
    assert(sp_data->ranks_bitset);
    GROUP_CACHE_BITSET_SET(sp_data->ranks_bitset, group_rank);

    // Same idea for the host
    host_data = GET_GROUP_HOST_HASH_ENTRY(gp_cache, host_uid);
    if (host_data == NULL)
    {
        // The host is not in the hash yet
        host_info_t *host_info = NULL;
        DBG("group cache does not have host 0x%lx, adding host to hash for the group (0x%x)",
            host_uid, gp_cache->group_uid);
        gp_cache->n_hosts++;
        // Add the SP to the hash using the global SP id as key
        assert(engine->free_host_cache_hash_obj);
        DYN_LIST_GET(engine->free_host_cache_hash_obj,
                     host_cache_data_t,
                     item,
                     host_data);
        assert(host_data);
        RESET_HOST_CACHE_DATA(host_data);
        host_data->uid = host_uid;
        host_data->num_ranks = 1;
        host_data->num_sps = 1;
        GROUP_CACHE_BITSET_CREATE(host_data->sps_bitset, gp_cache->group_size);
        GROUP_CACHE_BITSET_SET(host_data->sps_bitset, sp_gid);
        GROUP_CACHE_BITSET_CREATE(host_data->ranks_bitset, gp_cache->group_size);
        ADD_GROUP_HOST_HASH_ENTRY(gp_cache, host_data);
        host_info = LOOKUP_HOST_CONFIG(engine, host_uid);
        assert(host_info);
        host_data->config_idx = host_info->idx;
        GROUP_CACHE_BITSET_CREATE(gp_cache->hosts_bitset, engine->config->num_hosts);
        GROUP_CACHE_BITSET_SET(gp_cache->hosts_bitset,
                               host_info->idx);
    }
    else
    {
        // The host is already in the hash
        host_data->num_ranks++;
        if (!GROUP_CACHE_BITSET_TEST(host_data->sps_bitset, sp_gid))
        {
            // The SP is not known yet as being involved in the group
            host_data->num_sps++;
            GROUP_CACHE_BITSET_SET(host_data->sps_bitset, sp_gid);
        }
    }
    // Mark the rank as being part of the group and running on the host
    GROUP_CACHE_BITSET_SET(host_data->ranks_bitset, group_rank);

    return DO_SUCCESS;
}

dpu_offload_status_t
host_add_local_rank_to_cache(offloading_engine_t *engine, rank_info_t *rank_info)
{
    dpu_offload_state_t ret;
    peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache),
                                                                 rank_info->group_uid,
                                                                 rank_info->group_rank,
                                                                 rank_info->group_size);
    group_cache_t *gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), rank_info->group_uid);
    assert(cache_entry);
    assert(gp_cache);
    assert(engine->config != NULL);
    if (gp_cache->num_local_entries == 0)
    {
        // This is the first known rank for the group of the first group, set the group's sequence number to 1
        assert(rank_info->group_seq_num);
        gp_cache->persistent.num = rank_info->group_seq_num;
    }
    cache_entry->shadow_service_procs[cache_entry->num_shadow_service_procs] = engine->config->local_service_proc.info.global_id;
    cache_entry->peer.proc_info.group_uid = rank_info->group_uid;
    cache_entry->peer.proc_info.group_rank = rank_info->group_rank;
    cache_entry->peer.proc_info.group_size = rank_info->group_size;
    cache_entry->peer.proc_info.n_local_ranks = rank_info->n_local_ranks;
    cache_entry->peer.host_info = rank_info->host_info;
    cache_entry->num_shadow_service_procs++;
    cache_entry->set = true;
    gp_cache->num_local_entries++;

    ret = update_topology_data(engine, gp_cache, rank_info->group_rank, engine->config->local_service_proc.info.global_id, rank_info->host_info);
    CHECK_ERR_RETURN((ret != DO_SUCCESS), DO_ERROR, "update_topology_data() failed");

    return DO_SUCCESS;
}

static dpu_offload_status_t
do_get_cache_entry_by_group_rank(offloading_engine_t *engine,
                                 group_uid_t gp_uid,
                                 int64_t rank,
                                 int64_t sp_idx,
                                 request_compl_cb_t cb,
                                 int64_t *sp_global_id,
                                 dpu_offload_event_t **ev)
{
    if (ev != NULL && cb != NULL)
    {
        ERR_MSG("%s(): both the event and the callback are defined, impossible to understand the context", __func__);
        return DO_ERROR;
    }

    // If the event is defined, the dpu_id must also be defined, they go in pairs
    if (ev != NULL)
        assert(sp_global_id);

    if (is_in_cache(&(engine->procs_cache), gp_uid, rank, -1))
    {
        // The cache has the data
        peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache), gp_uid, rank, GROUP_SIZE_UNKNOWN);
        DBG("%" PRId64 " from group 0x%x is in the cache, service proc ID = %" PRId64, rank, gp_uid, cache_entry->shadow_service_procs[sp_idx]);
        if (ev != NULL)
        {
            *ev = NULL;
            *sp_global_id = cache_entry->shadow_service_procs[sp_idx];
        }
        return DO_SUCCESS;
    }

#if !NDEBUG
    // With the current implementation, it should always be in the cache
    WARN_MSG("rank %" PRId64 " from group 0x%x is not in the cache", rank, gp_uid);
    display_group_cache(&(engine->procs_cache), gp_uid);
    assert(0);
#endif

    // If the element is not in the cache, we send a request to the service process to get the data.
    // At the moment we assume by design that the cache is fully populated early on so we should never
    // face this situation. We keep the code in case we relax the assumptions later so we won't have
    // to start from scratch.
    abort();

    // The cache does not have the data. We sent a request to get the data.
    // The caller is in charge of calling the function after completion to actually get the data
    rank_info_t rank_data;
    RESET_RANK_INFO(&rank_data);
    rank_data.group_uid = gp_uid;
    rank_data.group_rank = rank;

    // Create the local event so we can know when the cache entry has been received
    dpu_offload_event_t *cache_entry_updated_ev;
    peer_cache_entry_t *cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(engine->procs_cache), gp_uid, rank, GROUP_SIZE_UNKNOWN);
    dpu_offload_status_t rc = event_get(engine->self_econtext->event_channels, NULL, &cache_entry_updated_ev);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
    if (!cache_entry->events_initialized)
    {
        SIMPLE_LIST_INIT(&(cache_entry->events));
        cache_entry->events_initialized = true;
    }
    EVENT_HDR_TYPE(cache_entry_updated_ev) = META_EVENT_TYPE;
    // We just queue a local event on the list for the cache entry to track what is being done in the
    // context of that entry
    SIMPLE_LIST_PREPEND(&(cache_entry->events), &(cache_entry_updated_ev->item));
    DBG("Cache entry %p for gp/rank 0x%x/%" PRIu64 " now has %ld update events",
        cache_entry, gp_uid, rank, SIMPLE_LIST_LENGTH(&(cache_entry->events)));
    if (ev != NULL)
    {
        // If the calling function is expecting an event back, no need for anything other than
        // make sure we return the event
        *ev = cache_entry_updated_ev;
    }
    if (cb != NULL)
    {
        // If the calling function specified a callback, the event needs to be hidden from the
        // caller and the callback will be invoked upon completion. So we need to put the event
        // on the list of ongoing events. Make sure to never return the event to the caller to
        // prevent the case where the event could be put on two different lists (which leads
        // to memory corruptions).
        cache_entry_request_t *request_data;
        DYN_LIST_GET(engine->free_cache_entry_requests, cache_entry_request_t, item, request_data);
        assert(request_data);
        request_data->gp_uid = gp_uid;
        request_data->rank = rank;
        request_data->target_sp_idx = sp_idx;
        request_data->offload_engine = engine;
        cache_entry_updated_ev->ctx.completion_cb = cb;
        cache_entry_updated_ev->ctx.completion_cb_ctx = (void *)request_data;
        assert(0); // FIXME: events cannot be on two lists
        // ucs_list_add_tail(&(engine->self_econtext->ongoing_events), &(cache_entry_updated_ev->item));
    }

    if (engine->on_dpu == true)
    {
        // If we are on a DPU, we need to send a request to all known DPUs
        // To track completion, we get an event from the execution context used for the
        // first DPU.
        size_t i;
        dpu_offload_status_t rc;
        dpu_offload_event_t *metaev = NULL;
        execution_context_t *meta_econtext = NULL;

        for (i = 0; i < engine->num_service_procs; i++)
        {
            remote_service_proc_info_t *sp;
            sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(engine), i, remote_service_proc_info_t);
            assert(sp);
            if (sp != NULL && sp->ep != NULL && sp->init_params.conn_params != NULL)
            {
                execution_context_t *econtext = ECONTEXT_FOR_SERVICE_PROC_COMMUNICATION(engine, i);
                CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "unable to get execution context to communicate with service process #%ld", i);
                uint64_t global_sp_id = LOCAL_ID_TO_GLOBAL(econtext, i);
                DBG("Sending cache entry request to service process #%ld (econtext: %p, scope_id: %d)",
                    global_sp_id,
                    econtext,
                    econtext->scope_id);

                if (metaev == NULL)
                {
                    meta_econtext = econtext;
                    rc = event_get(meta_econtext->event_channels, NULL, &metaev);
                    CHECK_ERR_RETURN((rc), DO_ERROR, "get_event() failed");
                    EVENT_HDR_TYPE(metaev) = META_EVENT_TYPE;
                }

                ucp_ep_h dpu_ep = sp->ep;
                dpu_offload_event_t *subev;
                rc = event_get(econtext->event_channels, NULL, &subev);
                CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");
                subev->is_subevent = true;
                rc = do_send_cache_entry_request(econtext, dpu_ep, i, &rank_data, subev);
                CHECK_ERR_RETURN((rc), DO_ERROR, "send_cache_entry_request() failed");
                DBG("Sub-event for sending cache to DPU %ld: %p", global_sp_id, subev);
                if (subev != NULL)
                {
                    // If the event did not complete right away, we add it as a sub-event to the meta-event so we can track everything
                    QUEUE_SUBEVENT(metaev, subev);
                }
            }
        }
        if (metaev)
        {
            assert(meta_econtext);
            if (!event_completed(metaev))
                QUEUE_EVENT(metaev);
            else
                event_return(&metaev);
        }
    }
    else
    {
        // If we are on the host, we need to send a request to our first shadow DPU
        DBG("Sending request for cache entry...");
        execution_context_t *econtext = engine->client;
        return send_cache_entry_request(econtext, GET_SERVER_EP(econtext), econtext->client->server_id, &rank_data, ev);
    }
    return DO_SUCCESS;
}

dpu_offload_status_t get_cache_entry_by_group_rank(offloading_engine_t *engine, group_uid_t gp_uid, int64_t rank, int64_t sp_idx, request_compl_cb_t cb)
{
    return do_get_cache_entry_by_group_rank(engine, gp_uid, rank, sp_idx, cb, NULL, NULL);
}

dpu_offload_status_t get_sp_id_by_group_rank(offloading_engine_t *engine, group_uid_t gp_uid, int64_t rank, int64_t sp_idx, int64_t *sp_id, dpu_offload_event_t **ev)
{
    return do_get_cache_entry_by_group_rank(engine, gp_uid, rank, sp_idx, NULL, sp_id, ev);
}


bool on_same_host(offloading_engine_t *engine,
                  group_uid_t gp_uid,
                  int64_t rank1,
                  int64_t rank2)
{
    uint64_t host1, host2;
    dpu_offload_status_t rc;
    assert(engine);
    rc = get_group_rank_host(engine, gp_uid, rank1, &host1);
    if (rc != DO_SUCCESS)
        return false;
    rc = get_group_rank_host(engine, gp_uid, rank2, &host2);
    if (rc != DO_SUCCESS)
        return false;
    if (host1 == host2)
        return true;
    return false;
}

bool on_same_sp(offloading_engine_t *engine,
                group_uid_t gp_uid,
                int64_t rank1,
                int64_t rank2)
{
    size_t rank1_sp_idx;
    group_cache_t *gp_cache = NULL;
    peer_cache_entry_t *rank1_data = NULL;
    assert(engine);

    // Get rank1's SPs data from the hash
    gp_cache = GET_GROUP_CACHE(&(engine->procs_cache), gp_uid);
    assert(gp_cache);

    if (!gp_cache->lookup_tables_populated)
    {
        dpu_offload_status_t rc;
        rc = do_populate_group_cache_lookup_table(engine, gp_cache);
        if (rc != DO_SUCCESS)
        {
            ERR_MSG("ERROR: populate_group_cache_lookup_table() failed: %d\n", rc);
        }
    }

    rank1_data = DYN_ARRAY_GET_ELT(&(gp_cache->ranks), rank1, peer_cache_entry_t);
    assert(rank1_data);

    for (rank1_sp_idx = 0; rank1_sp_idx < rank1_data->num_shadow_service_procs; rank1_sp_idx++)
    {
        sp_cache_data_t *sp_info = NULL;
        sp_info = GET_GROUP_SP_HASH_ENTRY(gp_cache, rank1_data->shadow_service_procs[rank1_sp_idx]);
        assert(sp_info);

        // Check if rank2 is the bitset of the ranks associated to the service process
        if (GROUP_CACHE_BITSET_TEST(sp_info->ranks_bitset, rank2))
            return true;
    }
    
    return false;
}
