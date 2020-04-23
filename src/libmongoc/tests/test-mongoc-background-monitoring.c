/*
 * Copyright 2020-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mock_server/mock-server.h"
#include "mongoc/mongoc.h"
#include "mongoc/mongoc-client-private.h"
#include "mongoc/mongoc-topology-private.h"
#include "mongoc/mongoc-topology-background-monitoring-private.h"
#include "mongoc/mongoc-topology-description-private.h"
#include "mongoc/mongoc-server-description-private.h"
#include "mongoc/mongoc-client-pool-private.h"
#include "test-libmongoc.h"
#include "TestSuite.h"

#define LOG_DOMAIN "test_monitoring"

typedef struct {
   uint32_t n_heartbeat_started;
   uint32_t n_heartbeat_succeeded;
   uint32_t n_heartbeat_failed;
   uint32_t n_server_changed;
   mongoc_topology_description_type_t td_type;
   mongoc_server_description_type_t sd_type;
} tf_observations_t;

typedef enum {
   TF_FAST_HEARTBEAT = 1 << 0,
   TF_FAST_MIN_HEARTBEAT = 1 << 1
} tf_flags_t;

typedef struct {
   tf_flags_t flags;
   mock_server_t *server;
   mongoc_client_pool_t *pool;
   mongoc_client_t *client;
   tf_observations_t *observations;
   bson_mutex_t mutex;
   mongoc_cond_t cond;
   bson_string_t *logs;
} test_fixture_t;

void
tf_dump (test_fixture_t *tf)
{
   printf ("== Begin dump ==\n");
   printf ("-- Current observations --\n");
   printf ("n_heartbeat_started=%d\n", tf->observations->n_heartbeat_started);
   printf ("n_heartbeat_succeeded=%d\n",
           tf->observations->n_heartbeat_succeeded);
   printf ("n_heartbeat_failed=%d\n", tf->observations->n_heartbeat_failed);
   printf ("n_server_changed=%d\n", tf->observations->n_server_changed);
   printf ("sd_type=%d\n", tf->observations->sd_type);

   printf ("-- Test fixture logs --\n");
   printf ("%s", tf->logs->str);
   printf ("== End dump ==\n");
}

void
tf_log (test_fixture_t *tf, const char *format, ...) BSON_GNUC_PRINTF (2, 3);

void
tf_log (test_fixture_t *tf, const char *format, ...)
{
   va_list ap;
   char *str;
   char nowstr[32];
   struct timeval tv;
   struct tm tt;
   time_t t;

   bson_gettimeofday (&tv);
   t = tv.tv_sec;

#ifdef _WIN32
#ifdef _MSC_VER
   localtime_s (&tt, &t);
#else
   tt = *(localtime (&t));
#endif
#else
   localtime_r (&t, &tt);
#endif

   strftime (nowstr, sizeof nowstr, "%Y/%m/%d %H:%M:%S ", &tt);

   va_start (ap, format);
   str = bson_strdupv_printf (format, ap);
   va_end (ap);
   bson_string_append (tf->logs, nowstr);
   bson_string_append (tf->logs, str);
   bson_string_append_c (tf->logs, '\n');
   bson_free (str);
}

#define TF_LOG(_tf, ...) tf_log (_tf, __VA_ARGS__)

static void
_heartbeat_started (const mongoc_apm_server_heartbeat_started_t *event)
{
   test_fixture_t *tf;

   tf = (test_fixture_t *) mongoc_apm_server_heartbeat_started_get_context (
      event);
   bson_mutex_lock (&tf->mutex);
   tf->observations->n_heartbeat_started++;
   TF_LOG (tf, "heartbeat started");
   mongoc_cond_broadcast (&tf->cond);
   bson_mutex_unlock (&tf->mutex);
}

static void
_heartbeat_succeeded (const mongoc_apm_server_heartbeat_succeeded_t *event)
{
   test_fixture_t *tf;

   tf = (test_fixture_t *) mongoc_apm_server_heartbeat_succeeded_get_context (
      event);
   bson_mutex_lock (&tf->mutex);
   tf->observations->n_heartbeat_succeeded++;
   TF_LOG (tf, "heartbeat succeeded");
   mongoc_cond_broadcast (&tf->cond);
   bson_mutex_unlock (&tf->mutex);
}

static void
_heartbeat_failed (const mongoc_apm_server_heartbeat_failed_t *event)
{
   test_fixture_t *tf;

   tf =
      (test_fixture_t *) mongoc_apm_server_heartbeat_failed_get_context (event);
   bson_mutex_lock (&tf->mutex);
   TF_LOG (tf, "heartbeat failed");
   tf->observations->n_heartbeat_failed++;
   mongoc_cond_broadcast (&tf->cond);
   bson_mutex_unlock (&tf->mutex);
}

static void
_server_changed (const mongoc_apm_server_changed_t *event)
{
   test_fixture_t *tf;
   const mongoc_server_description_t *new_sd;

   tf = (test_fixture_t *) mongoc_apm_server_changed_get_context (event);
   new_sd = mongoc_apm_server_changed_get_new_description (event);
   bson_mutex_lock (&tf->mutex);
   TF_LOG (tf, "server changed");
   tf->observations->sd_type = new_sd->type;
   tf->observations->n_server_changed++;
   mongoc_cond_broadcast (&tf->cond);
   bson_mutex_unlock (&tf->mutex);
}

test_fixture_t *
tf_new (tf_flags_t flags)
{
   mongoc_apm_callbacks_t *callbacks;
   test_fixture_t *tf;

   tf = bson_malloc0 (sizeof (test_fixture_t));
   tf->observations = bson_malloc0 (sizeof (tf_observations_t));
   bson_mutex_init (&tf->mutex);
   mongoc_cond_init (&tf->cond);

   callbacks = mongoc_apm_callbacks_new ();
   tf->server = mock_server_new ();
   mock_server_run (tf->server);

   mongoc_apm_set_server_heartbeat_started_cb (callbacks, _heartbeat_started);
   mongoc_apm_set_server_changed_cb (callbacks, _server_changed);
   mongoc_apm_set_server_heartbeat_succeeded_cb (callbacks,
                                                 _heartbeat_succeeded);
   mongoc_apm_set_server_heartbeat_failed_cb (callbacks, _heartbeat_failed);
   tf->pool = mongoc_client_pool_new (mock_server_get_uri (tf->server));
   mongoc_client_pool_set_apm_callbacks (tf->pool, callbacks, tf);
   mongoc_apm_callbacks_destroy (callbacks);

   if (flags & TF_FAST_HEARTBEAT) {
      _mongoc_client_pool_get_topology (tf->pool)->description.heartbeat_msec =
         10;
      /* A fast heartbeat implies a fast min heartbeat. */
      flags |= TF_FAST_MIN_HEARTBEAT;
   }
   if (flags & TF_FAST_MIN_HEARTBEAT) {
      _mongoc_client_pool_get_topology (tf->pool)
         ->min_heartbeat_frequency_msec = 10;
   }
   tf->flags = flags;
   tf->logs = bson_string_new ("");
   tf->client = mongoc_client_pool_pop (tf->pool);
   return tf;
}

void
tf_destroy (test_fixture_t *tf)
{
   mock_server_destroy (tf->server);
   mongoc_client_pool_push (tf->pool, tf->client);
   mongoc_client_pool_destroy (tf->pool);
   bson_string_free (tf->logs, true);
   bson_mutex_destroy (&tf->mutex);
   mongoc_cond_destroy (&tf->cond);
   bson_free (tf->observations);
   bson_free (tf);
}

/* Wait for _predicate to become true over the next five seconds.
 * _predicate is only tested when observations change.
 * Upon failure, dumps logs and observations.
 */
#define OBSERVE_SOON(_tf, _predicate)                           \
   do {                                                         \
      int64_t _start_ms = bson_get_monotonic_time () / 1000;    \
      int64_t _expires_ms = _start_ms + 5000;                   \
      bson_mutex_lock (&_tf->mutex);                            \
      while (!(_predicate)) {                                   \
         if (bson_get_monotonic_time () / 1000 > _expires_ms) { \
            bson_mutex_unlock (&_tf->mutex);                    \
            tf_dump (_tf);                                      \
            test_error ("Predicate expired: %s", #_predicate);  \
         }                                                      \
         mongoc_cond_timedwait (                                \
            &_tf->cond, &_tf->mutex, _expires_ms - _start_ms);  \
      }                                                         \
      bson_mutex_unlock (&_tf->mutex);                          \
   } while (0);

/* Check that _predicate is true immediately. Upon failure,
 * dumps logs and observations. */
#define OBSERVE(_tf, _predicate)                           \
   do {                                                    \
      bson_mutex_lock (&_tf->mutex);                       \
      if (!(_predicate)) {                                 \
         tf_dump (_tf);                                    \
         bson_mutex_unlock (&_tf->mutex);                  \
         test_error ("Predicate failed: %s", #_predicate); \
      }                                                    \
      bson_mutex_unlock (&_tf->mutex);                     \
   } while (0);

#define WAIT _mongoc_usleep (10 * 1000);

static void
_signal_shutdown (test_fixture_t *tf)
{
   bson_mutex_lock (&tf->client->topology->mutex);
   /* Ignore the "Last server removed from topology" warning. */
   capture_logs (true);
   /* remove the server description from the topology description. */
   mongoc_topology_description_reconcile (&tf->client->topology->description,
                                          NULL);
   capture_logs (false);
   /* remove the server monitor from the set of server monitors. */
   _mongoc_topology_background_monitoring_reconcile (tf->client->topology);
   bson_mutex_unlock (&tf->client->topology->mutex);
}

static void
_add_server_monitor (test_fixture_t *tf)
{
   uint32_t id;
   const mongoc_uri_t *uri;

   uri = mock_server_get_uri (tf->server);
   bson_mutex_lock (&tf->client->topology->mutex);
   /* remove the server description from the topology description. */
   mongoc_topology_description_add_server (
      &tf->client->topology->description,
      mongoc_uri_get_hosts (uri)->host_and_port,
      &id);
   /* add the server monitor from the set of server monitors. */
   _mongoc_topology_background_monitoring_reconcile (tf->client->topology);
   bson_mutex_unlock (&tf->client->topology->mutex);
}

static void
_request_scan (test_fixture_t *tf)
{
   bson_mutex_lock (&tf->client->topology->mutex);
   _mongoc_topology_request_scan (tf->client->topology);
   bson_mutex_unlock (&tf->client->topology->mutex);
}

void
test_connect_succeeds (void)
{
   test_fixture_t *tf;
   request_t *request;

   tf = tf_new (0);
   request = mock_server_receives_ismaster (tf->server);
   mock_server_replies_ok_and_destroys (request);

   OBSERVE_SOON (tf, tf->observations->n_heartbeat_started == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 0);
   OBSERVE_SOON (tf, tf->observations->n_server_changed == 1);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   tf_destroy (tf);
}

void
test_connect_hangup (void)
{
   test_fixture_t *tf;
   request_t *request;

   tf = tf_new (0);
   request = mock_server_receives_ismaster (tf->server);
   mock_server_hangs_up (request);
   request_destroy (request);

   OBSERVE_SOON (tf, tf->observations->n_heartbeat_started == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 0);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 1);
   OBSERVE_SOON (tf, tf->observations->n_server_changed == 0);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_UNKNOWN);

   /* No retry occurs since the server was never discovered. */
   WAIT;
   OBSERVE (tf, tf->observations->n_heartbeat_started == 1);
   tf_destroy (tf);
}

void
test_connect_badreply (void)
{
   test_fixture_t *tf;
   request_t *request;

   tf = tf_new (0);
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_replies_simple (request, "{'ok': 0}");
   request_destroy (request);

   OBSERVE_SOON (tf, tf->observations->n_heartbeat_started == 1);
   /* Still considered a successful heartbeat. */
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 0);
   OBSERVE_SOON (tf, tf->observations->n_server_changed == 0);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_UNKNOWN);

   /* No retry occurs since the server was never discovered. */
   WAIT;
   OBSERVE (tf, tf->observations->n_heartbeat_started == 1);
   tf_destroy (tf);
}

void
test_connect_shutdown (void)
{
   test_fixture_t *tf;
   request_t *request;

   tf = tf_new (0);
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   /* Before the server replies, signal the server monitor to shutdown. */
   _signal_shutdown (tf);

   /* Reply (or hang up) so the request does not wait for connectTimeoutMS to
    * time out. */
   mock_server_replies_ok_and_destroys (request);

   /* Heartbeat succeeds, but server description is not updated. */
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_started == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 0);
   OBSERVE_SOON (tf, tf->observations->n_server_changed == 0);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_UNKNOWN);

   tf_destroy (tf);
}

void
test_connect_requestscan (void)
{
   test_fixture_t *tf;
   request_t *request;

   tf = tf_new (0);
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   /* Before the mock server replies, request a scan. */
   _request_scan (tf);
   mock_server_replies_ok_and_destroys (request);

   /* Because the request occurred during the scan, no subsequent scan occurs.
    */
   WAIT;
   OBSERVE (tf, tf->observations->n_heartbeat_started == 1);
   OBSERVE (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE (tf, tf->observations->n_heartbeat_failed == 0);
   OBSERVE (tf, tf->observations->n_server_changed == 1);
   OBSERVE (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   tf_destroy (tf);
}

void
test_retry_succeeds (void)
{
   test_fixture_t *tf;
   request_t *request;

   tf = tf_new (TF_FAST_HEARTBEAT);

   /* Initial discovery occurs. */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_replies_ok_and_destroys (request);

   /* Heartbeat succeeds, but server description is not updated. */
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 0);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   /* The next ismaster occurs (due to fast heartbeat). */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_hangs_up (request);
   request_destroy (request);

   /* Server is still standalone. */
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 1);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   /* Retry occurs. */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_replies_ok_and_destroys (request);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 2);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 1);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   tf_destroy (tf);
}

void
test_retry_hangup (void)
{
   test_fixture_t *tf;
   request_t *request;

   tf = tf_new (TF_FAST_HEARTBEAT);

   /* Initial discovery occurs. */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_replies_ok_and_destroys (request);

   /* Heartbeat succeeds, but server description is not updated. */
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 0);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   /* The next ismaster occurs (due to fast heartbeat). */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_hangs_up (request);
   request_destroy (request);

   /* Server is still standalone. */
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 1);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   /* Retry occurs. */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_hangs_up (request);
   request_destroy (request);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 2);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_UNKNOWN);

   tf_destroy (tf);
}

void
test_retry_badreply (void)
{
   test_fixture_t *tf;
   request_t *request;

   tf = tf_new (TF_FAST_HEARTBEAT);

   /* Initial discovery occurs. */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_replies_ok_and_destroys (request);

   /* Heartbeat succeeds, but server description is not updated. */
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 0);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   /* The next ismaster occurs (due to fast heartbeat). */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_hangs_up (request);
   request_destroy (request);

   /* Server is still standalone. */
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 1);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   /* Retry occurs. */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_replies_simple (request, "{'ok': 0}");
   request_destroy (request);
   /* Heartbeat succeeds, but server description is unknown. */
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 2);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 1);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_UNKNOWN);

   tf_destroy (tf);
}

void
test_retry_shutdown (void)
{
   test_fixture_t *tf;
   request_t *request;

   tf = tf_new (TF_FAST_HEARTBEAT);

   /* Initial discovery occurs. */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   mock_server_replies_ok_and_destroys (request);

   /* Heartbeat succeeds, but server description is not updated. */
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == 1);
   OBSERVE_SOON (tf, tf->observations->n_heartbeat_failed == 0);
   OBSERVE_SOON (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   /* The next ismaster occurs (due to fast heartbeat). */
   request = mock_server_receives_ismaster (tf->server);
   OBSERVE (tf, request);
   _signal_shutdown (tf);
   mock_server_replies_ok_and_destroys (request);

   /* No retry occurs. */
   WAIT;
   OBSERVE (tf, tf->observations->n_heartbeat_started == 2);
   OBSERVE (tf, tf->observations->n_heartbeat_succeeded == 2);
   OBSERVE (tf, tf->observations->n_heartbeat_failed == 0);
   OBSERVE (tf, tf->observations->sd_type == MONGOC_SERVER_STANDALONE);

   tf_destroy (tf);
}

static void
test_flip_flop (void)
{
   test_fixture_t *tf;
   request_t *request;
   int i;

   tf = tf_new (0);

   for (i = 1; i < 100; i++) {
      request = mock_server_receives_ismaster (tf->server);
      OBSERVE (tf, request);
      mock_server_replies_ok_and_destroys (request);
      _signal_shutdown (tf);
      OBSERVE_SOON (tf, tf->observations->n_heartbeat_started == i);
      OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == i);
      _add_server_monitor (tf);
   }

   tf_destroy (tf);
}

static void
test_repeated_requestscan (void)
{
   test_fixture_t *tf;
   request_t *request;
   int i;

   tf = tf_new (TF_FAST_MIN_HEARTBEAT);

   for (i = 1; i < 100; i++) {
      request = mock_server_receives_ismaster (tf->server);
      OBSERVE (tf, request);
      mock_server_replies_ok_and_destroys (request);
      OBSERVE_SOON (tf, tf->observations->n_heartbeat_started == i);
      OBSERVE_SOON (tf, tf->observations->n_heartbeat_succeeded == i);
      _request_scan (tf);
   }

   tf_destroy (tf);
}

void
test_monitoring_install (TestSuite *suite)
{
   /* Tests for initial connection. */
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/connect/succeeds", test_connect_succeeds);
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/connect/hangup", test_connect_hangup);
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/connect/badreply", test_connect_badreply);
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/connect/shutdown", test_connect_shutdown);
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/connect/requestscan", test_connect_requestscan);

   /* Tests for retry. */
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/retry/succeeds", test_retry_succeeds);
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/retry/hangup", test_retry_hangup);
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/retry/badreply", test_retry_badreply);
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/retry/shutdown", test_retry_shutdown);

   /* Test flip flopping. */
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/flip_flop", test_flip_flop);

   /* Test repeated scan requests. */
   TestSuite_AddMockServerTest (
      suite, "/server_monitor/repeated_requestscan", test_repeated_requestscan);
}
