//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"

static inline bool req_completed(struct ucx_context *req)
{
    if (req == NULL)
        return true;

    ucs_status_t status = ucp_request_check_status(req);
    if (status == UCS_INPROGRESS)
        return false;
    return true;
}

static void recv_cb(void *request, ucs_status_t status, ucp_tag_recv_info_t *info)
{
    fprintf(stderr, "ping msg from client successfully received, sending pong...\n");
}

static void send_cb(void *request, ucs_status_t status)
{
    fprintf(stderr, "pong successfully sent\n");
}

bool notification_recvd = false;
static int dummy_notification_cb(struct dpu_offload_ev_sys *ev_sys, void *context, void *data)
{
    fprintf(stderr, "Notification successfully received\n");
    notification_recvd = true;
}

int main(int argc, char **argv)
{
    offloading_engine_t *offload_engine;
    int rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        return EXIT_FAILURE;
    }

    execution_context_t *server = server_init(offload_engine, NULL);
    if (server == NULL)
    {
        fprintf(stderr, "server handle is undefined\n");
        return EXIT_FAILURE;
    }

    // REGISTER SOME EVENTS FOR TESTING
    fprintf(stderr, "Registering callback for notifications of type %d\n", AM_TEST_MSG_ID);
    rc = event_channel_register(server->event_channels, AM_TEST_MSG_ID, dummy_notification_cb);
    if (rc)
    {
        fprintf(stderr, "event_channel_register() failed\n");
        return EXIT_FAILURE;
    }

    // PING_PONG TEST
    int msg_tag = 42;
    ucp_tag_t msg_tag_mask = (ucp_tag_t)-1;
    int ping;
    struct ucx_context *recv_req = ucp_tag_recv_nb(GET_WORKER(server), &ping, sizeof(ping), ucp_dt_make_contig(1), msg_tag, msg_tag_mask, recv_cb);
    if (UCS_PTR_IS_ERR(recv_req))
    {
        fprintf(stderr, "Recv failed\n");
        ucp_request_cancel(GET_WORKER(server), recv_req);
        ucp_request_free(recv_req);
        recv_req = NULL;
    }
    /* Did the request complete right away? */
    ucp_tag_recv_info_t _info;
    ucs_status_t _status = ucp_tag_recv_request_test(recv_req, &_info);
    if (_status != UCS_INPROGRESS)
    {
        ucp_request_free(recv_req);
        recv_req = NULL;
    }
    if (recv_req != NULL)
    {
        while (!req_completed(recv_req))
            server->progress(server);
        ucp_request_free(recv_req);
        recv_req = NULL;
    }

    int msg = ping + 1;
    ucp_ep_h ep = server->server->connected_clients.clients[0].ep;
    struct ucx_context *send_req = ucp_tag_send_nb(ep, &msg, sizeof(msg), ucp_dt_make_contig(1), msg_tag, send_cb);
    if (UCS_PTR_IS_ERR(send_req))
    {
        ucp_request_cancel(GET_WORKER(server), send_req);
        ucp_request_free(send_req);
        send_req = NULL;
    }
    if (send_req != NULL)
    {
        while (!req_completed(send_req))
           server->progress(server);
        ucp_request_free(send_req);
        send_req = NULL;
    }

    while (!notification_recvd)
    {
        server->progress(server);
    }

    fprintf(stderr, "ALL TESTS COMPLETED\n");

    fprintf(stderr, "Waiting for client to terminate...\n");

    while (!EXECUTION_CONTEXT_DONE(server))
    {
        server->progress(server);
    }

    server_fini(&server);
    offload_engine_fini(&offload_engine);

    fprintf(stderr, "server all done, exiting successfully\n");
    return EXIT_SUCCESS;
}