//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_types.h"

#ifndef DPU_OFFLOAD_EVENT_CHANNELS_H_
#define DPU_OFFLOAD_EVENT_CHANNELS_H_

dpu_offload_status_t event_channels_init(execution_context_t *);
dpu_offload_status_t event_channel_register(dpu_offload_ev_sys_t *ev_sys, uint64_t type, notification_cb cb);
dpu_offload_status_t event_channel_deregister(dpu_offload_ev_sys_t *ev_sys, uint64_t type);

/**
 * @brief event_channel_emit triggers the communication associated to a previously locally defined event.
 * 
 * @param ev Event to be emitted. The object needs to be fully initialized prior the invokation of the function (see 'event_get()' and 'event_return()').
 * @param my_id The unique identifier to be used to send the event. It is used to identify the source of the event.
 * @param type Event type, i.e., identifier of the callback to invoke when the event is delivered at destination.
 * @param dest_ep Endpoint of the target of the event.
 * @param ctx User-defined context to help identify the context of the event upon local completion.
 * @param payload User-defined payload associated to the event.
 * @param payload_size Size of the user-defined payload.
 * @return result of UCS_PTR_STATUS in the context of errors, EVENT_DONE if the emittion completed right away, EVENT_INPROGRESS if emitting the event is still in progress (e.g., communication not completed). One can check on completion using ev->req.
 */
int event_channel_emit(dpu_offload_event_t *ev, uint64_t my_id, uint64_t type, ucp_ep_h dest_ep, void *ctx, void *payload, size_t payload_size);

dpu_offload_status_t event_channels_fini(dpu_offload_ev_sys_t **);

dpu_offload_status_t event_get(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t **ev);
dpu_offload_status_t event_return(dpu_offload_ev_sys_t *ev_sys, dpu_offload_event_t **ev);

#endif // DPU_OFFLOAD_EVENT_CHANNELS_H_
