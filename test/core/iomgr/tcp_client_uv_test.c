/*
 *
 * Copyright 2017, Google Inc.
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

#include "src/core/lib/iomgr/port.h"

// This test won't work except with libuv
#ifdef GRPC_UV

#include <uv.h>

#include <string.h>

#include "src/core/lib/iomgr/tcp_client.h"

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>

#include "src/core/lib/iomgr/iomgr.h"
#include "src/core/lib/iomgr/pollset.h"
#include "src/core/lib/iomgr/timer.h"
#include "test/core/util/test_config.h"

static gpr_mu *g_mu;
static grpc_pollset *g_pollset;
static int g_connections_complete = 0;
static grpc_endpoint *g_connecting = NULL;

static gpr_timespec test_deadline(void) {
  return GRPC_TIMEOUT_SECONDS_TO_DEADLINE(10);
}

static void finish_connection() {
  gpr_mu_lock(g_mu);
  g_connections_complete++;
  GPR_ASSERT(
      GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(g_pollset, NULL)));
  gpr_mu_unlock(g_mu);
}

static void must_succeed(grpc_exec_ctx *exec_ctx, void *arg,
                         grpc_error *error) {
  GPR_ASSERT(g_connecting != NULL);
  GPR_ASSERT(error == GRPC_ERROR_NONE);
  grpc_endpoint_shutdown(exec_ctx, g_connecting);
  grpc_endpoint_destroy(exec_ctx, g_connecting);
  g_connecting = NULL;
  finish_connection();
}

static void must_fail(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error) {
  GPR_ASSERT(g_connecting == NULL);
  GPR_ASSERT(error != GRPC_ERROR_NONE);
  finish_connection();
}

static void close_cb(uv_handle_t *handle) { gpr_free(handle); }

static void connection_cb(uv_stream_t *server, int status) {
  uv_tcp_t *client_handle = gpr_malloc(sizeof(uv_tcp_t));
  GPR_ASSERT(0 == status);
  GPR_ASSERT(0 == uv_tcp_init(uv_default_loop(), client_handle));
  GPR_ASSERT(0 == uv_accept(server, (uv_stream_t *)client_handle));
  uv_close((uv_handle_t *)client_handle, close_cb);
}

void test_succeeds(void) {
  grpc_resolved_address resolved_addr;
  struct sockaddr_in *addr = (struct sockaddr_in *)resolved_addr.addr;
  uv_tcp_t *svr_handle = gpr_malloc(sizeof(uv_tcp_t));
  int connections_complete_before;
  grpc_closure done;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;

  gpr_log(GPR_DEBUG, "test_succeeds");

  memset(&resolved_addr, 0, sizeof(resolved_addr));
  resolved_addr.len = sizeof(struct sockaddr_in);
  addr->sin_family = AF_INET;

  /* create a dummy server */
  GPR_ASSERT(0 == uv_tcp_init(uv_default_loop(), svr_handle));
  GPR_ASSERT(0 == uv_tcp_bind(svr_handle, (struct sockaddr *)addr, 0));
  GPR_ASSERT(0 == uv_listen((uv_stream_t *)svr_handle, 1, connection_cb));

  gpr_mu_lock(g_mu);
  connections_complete_before = g_connections_complete;
  gpr_mu_unlock(g_mu);

  /* connect to it */
  GPR_ASSERT(uv_tcp_getsockname(svr_handle, (struct sockaddr *)addr,
                                (int *)&resolved_addr.len) == 0);
  grpc_closure_init(&done, must_succeed, NULL, grpc_schedule_on_exec_ctx);
  grpc_tcp_client_connect(&exec_ctx, &done, &g_connecting, NULL, NULL,
                          &resolved_addr, gpr_inf_future(GPR_CLOCK_REALTIME));

  gpr_mu_lock(g_mu);

  while (g_connections_complete == connections_complete_before) {
    grpc_pollset_worker *worker = NULL;
    GPR_ASSERT(GRPC_LOG_IF_ERROR(
        "pollset_work",
        grpc_pollset_work(&exec_ctx, g_pollset, &worker,
                          gpr_now(GPR_CLOCK_MONOTONIC),
                          GRPC_TIMEOUT_SECONDS_TO_DEADLINE(5))));
    gpr_mu_unlock(g_mu);
    grpc_exec_ctx_flush(&exec_ctx);
    gpr_mu_lock(g_mu);
  }

  // This will get cleaned up when the pollset runs again or gets shutdown
  uv_close((uv_handle_t *)svr_handle, close_cb);

  gpr_mu_unlock(g_mu);

  grpc_exec_ctx_finish(&exec_ctx);
}

void test_fails(void) {
  grpc_resolved_address resolved_addr;
  struct sockaddr_in *addr = (struct sockaddr_in *)resolved_addr.addr;
  int connections_complete_before;
  grpc_closure done;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;

  gpr_log(GPR_DEBUG, "test_fails");

  memset(&resolved_addr, 0, sizeof(resolved_addr));
  resolved_addr.len = sizeof(struct sockaddr_in);
  addr->sin_family = AF_INET;

  gpr_mu_lock(g_mu);
  connections_complete_before = g_connections_complete;
  gpr_mu_unlock(g_mu);

  /* connect to a broken address */
  grpc_closure_init(&done, must_fail, NULL, grpc_schedule_on_exec_ctx);
  grpc_tcp_client_connect(&exec_ctx, &done, &g_connecting, NULL, NULL,
                          &resolved_addr, gpr_inf_future(GPR_CLOCK_REALTIME));

  gpr_mu_lock(g_mu);

  /* wait for the connection callback to finish */
  while (g_connections_complete == connections_complete_before) {
    grpc_pollset_worker *worker = NULL;
    gpr_timespec now = gpr_now(GPR_CLOCK_MONOTONIC);
    gpr_timespec polling_deadline = test_deadline();
    if (!grpc_timer_check(&exec_ctx, now, &polling_deadline)) {
      GPR_ASSERT(GRPC_LOG_IF_ERROR(
          "pollset_work", grpc_pollset_work(&exec_ctx, g_pollset, &worker, now,
                                            polling_deadline)));
    }
    gpr_mu_unlock(g_mu);
    grpc_exec_ctx_flush(&exec_ctx);
    gpr_mu_lock(g_mu);
  }

  gpr_mu_unlock(g_mu);
  grpc_exec_ctx_finish(&exec_ctx);
}

static void destroy_pollset(grpc_exec_ctx *exec_ctx, void *p,
                            grpc_error *error) {
  grpc_pollset_destroy(p);
}

int main(int argc, char **argv) {
  grpc_closure destroyed;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_test_init(argc, argv);
  grpc_init();
  g_pollset = gpr_malloc(grpc_pollset_size());
  grpc_pollset_init(g_pollset, &g_mu);
  grpc_exec_ctx_finish(&exec_ctx);
  test_succeeds();
  gpr_log(GPR_ERROR, "End of first test");
  test_fails();
  grpc_closure_init(&destroyed, destroy_pollset, g_pollset,
                    grpc_schedule_on_exec_ctx);
  grpc_pollset_shutdown(&exec_ctx, g_pollset, &destroyed);
  grpc_exec_ctx_finish(&exec_ctx);
  grpc_shutdown();
  gpr_free(g_pollset);
  return 0;
}

#else /* GRPC_UV */

int main(int argc, char **argv) { return 1; }

#endif /* GRPC_UV */
