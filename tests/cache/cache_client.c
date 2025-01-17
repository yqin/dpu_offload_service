//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <unistd.h>

#include "dpu_offload_service_daemon.h"
#include "test_cache_common.h"

/*
 * This test connects to a daemon, explicitely sends all the rank information
 * and waits for the cache to be locally populated. Once populated, we check
 * the content of the cache.
 */

int main(int argc, char **argv)
{
    /* Initialize everything we need for the test */
    offloading_engine_t *offload_engine;
    dpu_offload_status_t rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        goto error_out;
    }

    execution_context_t *client = client_init(offload_engine, NULL);
    if (client == NULL)
    {
        fprintf(stderr, "client handle is undefined\n");
        return EXIT_FAILURE;
    }
    ADD_CLIENT_TO_ENGINE(client, offload_engine);

    ECONTEXT_LOCK(client);
    int bootstrapping_status = client->client->bootstrapping.phase;
    ECONTEXT_UNLOCK(client);
    while (bootstrapping_status != BOOTSTRAP_DONE)
    {
        lib_progress(client);
        ECONTEXT_LOCK(client);
        bootstrapping_status = client->client->bootstrapping.phase;
        ECONTEXT_UNLOCK(client);
    }

    ucp_ep_h remote_ep = client->client->server_ep;
    if (remote_ep == NULL)
    {
        fprintf(stderr, "undefined destination endpoint\n");
        goto error_out;
    }

    dpu_offload_event_t *ev;
    rc = event_get(client->event_channels, NULL, &ev);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "event_get() failed\n");
        goto error_out;
    }
    if (ev == NULL)
    {
        fprintf(stderr, "undefined event\n");
        goto error_out;
    }
    
    rc = send_cache(client, &(offload_engine->procs_cache), remote_ep, client->client->server_id, ev);
    if (rc != DO_SUCCESS)
    {
        fprintf(stderr, "send_cache() failed\n");
        goto error_out;
    }

    /* Progress until the last element in the cache is set */
    fprintf(stderr, "Waiting for all the cache entries to arrive...\n");

    // todo: we should use the event to know when it is all completed
    int retry = 0;
    bool test_done = false;
    while (!test_done)
    {
        lib_progress(client);
        cache_t *cache = &(offload_engine->procs_cache);
        group_cache_t *gp_cache = GET_GROUP_CACHE(cache, default_gp_uid);
        if (gp_cache->initialized)
        {
            peer_cache_entry_t *list_ranks = (peer_cache_entry_t *)gp_cache->ranks.base;
            peer_data_t *target_peer = &(list_ranks[DEFAULT_NUM_RANKS - 1].peer);
            if (IS_A_VALID_PEER_DATA(target_peer))
                test_done = true;
        }
        sleep(1);
        retry++;
        if (retry == 5)
        {
            fprintf(stderr, "error: data still not received\n");
            break;
        }
    }

    /* Check we got all the expected data in the cache */
    CHECK_CACHE(offload_engine, default_gp_uid, DEFAULT_NUM_RANKS);

    event_return(&ev);

    client_fini(&client);
    offload_engine_fini(&offload_engine);
    fprintf(stdout, "%s: test successful\n", argv[0]);
    return EXIT_SUCCESS;

error_out:
    client_fini(&client);
    offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}