/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef __GRPC_INTERNAL_TRANSPORT_TRANSPORT_H__
#define __GRPC_INTERNAL_TRANSPORT_TRANSPORT_H__

#include <stddef.h>

#include "src/core/iomgr/pollset.h"
#include "src/core/transport/stream_op.h"

/* forward declarations */
typedef struct grpc_transport grpc_transport;
typedef struct grpc_transport_callbacks grpc_transport_callbacks;

/* grpc_stream doesn't actually exist. It's used as a typesafe
   opaque pointer for whatever data the transport wants to track
   for a stream. */
typedef struct grpc_stream grpc_stream;

/* Represents the send/recv closed state of a stream. */
typedef enum grpc_stream_state {
  /* the stream is open for sends and receives */
  GRPC_STREAM_OPEN,
  /* the stream is closed for sends, but may still receive data */
  GRPC_STREAM_SEND_CLOSED,
  /* the stream is closed for receives, but may still send data */
  GRPC_STREAM_RECV_CLOSED,
  /* the stream is closed for both sends and receives */
  GRPC_STREAM_CLOSED
} grpc_stream_state;

/* Callbacks made from the transport to the upper layers of grpc. */
struct grpc_transport_callbacks {
  /* Allocate a buffer to receive data into.
     It's safe to call grpc_slice_new() to do this, but performance minded
     proxies may want to carefully place data into optimal locations for
     transports.
     This function must return a valid, non-empty slice.

     Arguments:
       user_data - the transport user data set at transport creation time
       transport - the grpc_transport instance making this call
       stream    - the grpc_stream instance the buffer will be used for, or
                   NULL if this is not known
       size_hint - how big of a buffer would the transport optimally like?
                   the actual returned buffer can be smaller or larger than
                   size_hint as the implementation finds convenient */
  struct gpr_slice (*alloc_recv_buffer)(void *user_data,
                                        grpc_transport *transport,
                                        grpc_stream *stream, size_t size_hint);

  /* Initialize a new stream on behalf of the transport.
     Must result in a call to
     grpc_transport_init_stream(transport, ..., request) in the same call
     stack.
     Must not result in any other calls to the transport.

     Arguments:
       user_data     - the transport user data set at transport creation time
       transport     - the grpc_transport instance making this call
       request       - request parameters for this stream (owned by the caller)
       server_data   - opaque transport dependent argument that should be passed
                       to grpc_transport_init_stream
     */
  void (*accept_stream)(void *user_data, grpc_transport *transport,
                        const void *server_data);

  /* Process a set of stream ops that have been received by the transport.
     Called by network threads, so must be careful not to block on network
     activity.

     If final_state == GRPC_STREAM_CLOSED, the upper layers should arrange to
     call grpc_transport_destroy_stream.

     Ownership of any objects contained in ops is transferred to the callee.

     Arguments:
       user_data   - the transport user data set at transport creation time
       transport   - the grpc_transport instance making this call
       stream      - the stream this data was received for
       ops         - stream operations that are part of this batch
       ops_count   - the number of stream operations in this batch
       final_state - the state of the stream as of the final operation in this
                     batch */
  void (*recv_batch)(void *user_data, grpc_transport *transport,
                     grpc_stream *stream, grpc_stream_op *ops, size_t ops_count,
                     grpc_stream_state final_state);

  /* The transport received a goaway */
  void (*goaway)(void *user_data, grpc_transport *transport,
                 grpc_status_code status, gpr_slice debug);

  /* The transport has been closed */
  void (*closed)(void *user_data, grpc_transport *transport);
};

/* Returns the amount of memory required to store a grpc_stream for this
   transport */
size_t grpc_transport_stream_size(grpc_transport *transport);

/* Initialize transport data for a stream.

   Returns 0 on success, any other (transport-defined) value for failure.

   Arguments:
     transport   - the transport on which to create this stream
     stream      - a pointer to uninitialized memory to initialize
     server_data - either NULL for a client initiated stream, or a pointer
                   supplied from the accept_stream callback function */
int grpc_transport_init_stream(grpc_transport *transport, grpc_stream *stream,
                               const void *server_data);

/* Destroy transport data for a stream.

   Requires: a recv_batch with final_state == GRPC_STREAM_CLOSED has been
   received by the up-layer. Must not be called in the same call stack as
   recv_frame.

   Arguments:
     transport - the transport on which to create this stream
     stream    - the grpc_stream to destroy (memory is still owned by the
                 caller, but any child memory must be cleaned up) */
void grpc_transport_destroy_stream(grpc_transport *transport,
                                   grpc_stream *stream);

/* Enable/disable incoming data for a stream.

   This effectively disables new window becoming available for a given stream,
   but does not prevent existing window from being consumed by a sender: the
   caller must still be prepared to receive some additional data after this
   call.

   Arguments:
     transport - the transport on which to create this stream
     stream    - the grpc_stream to destroy (memory is still owned by the
                 caller, but any child memory must be cleaned up)
     allow     - is it allowed that new window be opened up? */
void grpc_transport_set_allow_window_updates(grpc_transport *transport,
                                             grpc_stream *stream, int allow);

/* Send a batch of operations on a transport

   Takes ownership of any objects contained in ops.

   Arguments:
     transport - the transport on which to initiate the stream
     stream    - the stream on which to send the operations. This must be
                 non-NULL and previously initialized by the same transport.
     ops       - an array of operations to apply to the stream - can be NULL
                 if ops_count == 0.
     ops_count - the number of elements in ops
     is_last   - is this the last batch of operations to be sent out */
void grpc_transport_send_batch(grpc_transport *transport, grpc_stream *stream,
                               grpc_stream_op *ops, size_t ops_count,
                               int is_last);

/* Send a ping on a transport

   Calls cb with user data when a response is received.
   cb *MAY* be called with arbitrary transport level locks held. It is not safe
   to call into the transport during cb. */
void grpc_transport_ping(grpc_transport *transport, void (*cb)(void *user_data),
                         void *user_data);

/* Abort a stream

   Terminate reading and writing for a stream. A final recv_batch with no
   operations and final_state == GRPC_STREAM_CLOSED will be received locally,
   and no more data will be presented to the up-layer.

   TODO(ctiller): consider adding a HTTP/2 reason to this function. */
void grpc_transport_abort_stream(grpc_transport *transport, grpc_stream *stream,
                                 grpc_status_code status);

void grpc_transport_add_to_pollset(grpc_transport *transport,
                                   grpc_pollset *pollset);

/* Advise peer of pending connection termination. */
void grpc_transport_goaway(grpc_transport *transport, grpc_status_code status,
                           gpr_slice debug_data);

/* Close a transport. Aborts all open streams. */
void grpc_transport_close(grpc_transport *transport);

/* Destroy the transport */
void grpc_transport_destroy(grpc_transport *transport);

/* Return type for grpc_transport_setup_callback */
typedef struct grpc_transport_setup_result {
  void *user_data;
  const grpc_transport_callbacks *callbacks;
} grpc_transport_setup_result;

/* Given a transport, return callbacks for that transport. Used to finalize
   setup as a transport is being created */
typedef grpc_transport_setup_result (*grpc_transport_setup_callback)(
    void *setup_arg, grpc_transport *transport, grpc_mdctx *mdctx);

typedef struct grpc_transport_setup grpc_transport_setup;
typedef struct grpc_transport_setup_vtable grpc_transport_setup_vtable;

struct grpc_transport_setup_vtable {
  void (*initiate)(grpc_transport_setup *setup);
  void (*cancel)(grpc_transport_setup *setup);
};

/* Transport setup is an asynchronous utility interface for client channels to
   establish connections. It's transport agnostic. */
struct grpc_transport_setup {
  const grpc_transport_setup_vtable *vtable;
};

/* Initiate transport setup: e.g. for TCP+DNS trigger a resolve of the name
   given at transport construction time, create the tcp connection, perform
   handshakes, and call some grpc_transport_setup_result function provided at
   setup construction time.
   This *may* be implemented as a no-op if the setup process monitors something
   continuously. */
void grpc_transport_setup_initiate(grpc_transport_setup *setup);
/* Cancel transport setup. After this returns, no new transports should be
   created, and all pending transport setup callbacks should be completed.
   After this call completes, setup should be considered invalid (this can be
   used as a destruction call by setup). */
void grpc_transport_setup_cancel(grpc_transport_setup *setup);

#endif /* __GRPC_INTERNAL_TRANSPORT_TRANSPORT_H__ */
