/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2021, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Seven Du <seven@signalwire.com>
 *
 *
 * test_sofia_funcs.c -- tests sofia functions
 *
 */

#include <switch.h>
#include <test/switch_test.h>
#include "../mod_sofia.h"

int protect_dest_uri(switch_caller_profile_t *cp);

static int timeout_sec = 10;
static switch_interval_time_t delay_start_ms = 5000;

typedef struct {
	switch_core_session_t *session;
	switch_core_session_message_types_t message_id;
	int numeric_arg;
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;
	int ready;
	int proceed;
	int send;
	int completed;
	switch_status_t status;
} media_notification_test_t;

static void *SWITCH_THREAD_FUNC send_media_notification(switch_thread_t *thread, void *obj)
{
	media_notification_test_t *test = obj;
	switch_core_session_message_t msg = { 0 };

	/* Match the write-frame path: notifications are sent with this lock held. */
	switch_core_session_lock_codec_write(test->session);
	switch_mutex_lock(test->mutex);
	test->ready = 1;
	switch_thread_cond_signal(test->cond);
	while (!test->proceed) {
		switch_thread_cond_wait(test->cond, test->mutex);
	}
	switch_mutex_unlock(test->mutex);
	if (test->send) {
		msg.message_id = test->message_id;
		msg.numeric_arg = test->numeric_arg;
		test->status = switch_core_session_receive_message(test->session, &msg);
	}
	switch_core_session_unlock_codec_write(test->session);

	switch_mutex_lock(test->mutex);
	test->completed = 1;
	switch_thread_cond_signal(test->cond);
	switch_mutex_unlock(test->mutex);
	return NULL;
}

FST_CORE_EX_BEGIN("./conf", SCF_VG | SCF_USE_SQL)

FST_MODULE_BEGIN(mod_sofia, sofia)

FST_SETUP_BEGIN()
{
	/* Give mod_sofia time to spinup profile threads */
	if (delay_start_ms) {
		switch_sleep(delay_start_ms * 1000);
		delay_start_ms = 0;
	}
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()

FST_TEST_BEGIN(test_protect_url)
{
	int ret;
	switch_caller_profile_t cp = { 0 };
	switch_core_new_memory_pool(&cp.pool);

	cp.destination_number = switch_core_strdup(cp.pool, "1234");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 0);
	fst_check_string_equals(cp.destination_number, "1234");

	cp.destination_number = switch_core_strdup(cp.pool, "1234@ip");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 0);
	fst_check_string_equals(cp.destination_number, "1234@ip");

	cp.destination_number = switch_core_strdup(cp.pool, "external/1234@ip");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 0);
	fst_check_string_equals(cp.destination_number, "external/1234@ip");

	cp.destination_number = switch_core_strdup(cp.pool, "external/sip:1234@ip");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 0);
	fst_check_string_equals(cp.destination_number, "external/1234@ip");

	cp.destination_number = switch_core_strdup(cp.pool, "external/sips:1234@ip");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 0);
	fst_check_string_equals(cp.destination_number, "external/1234@ip");

	cp.destination_number = switch_core_strdup(cp.pool, "external/bryän&!杜金房@freeswitch-testing:9080");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 1);
	fst_check_string_equals(cp.destination_number, "external/bry%C3%A4n%26!%E6%9D%9C%E9%87%91%E6%88%BF@freeswitch-testing:9080");

	cp.destination_number = switch_core_strdup(cp.pool, "external/" SWITCH_URL_UNSAFE "@freeswitch-testing:9080");
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "before: [%s]\n", cp.destination_number);
	ret = protect_dest_uri(&cp);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "after: [%s]\n", cp.destination_number);
	fst_check(ret == 1);
	fst_check_string_equals(cp.destination_number, "external/%0D%0A%20%23%25%26%2B%3A%3B%3C%3D%3E%3F@[\\]^`{|}\"@freeswitch-testing:9080");

	switch_core_destroy_memory_pool(&cp.pool);
}
FST_TEST_END()

FST_TEST_BEGIN(originate_test)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{ignore_early_media=true}sofia/internal/park@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(session);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(media_notifications_do_not_wait_for_sofia_mutex)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause;
	switch_status_t status;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");

	status = switch_ivr_originate(NULL, &session, &cause,
		switch_core_sprintf(fst_pool, "{ignore_early_media=true}sofia/internal/park@%s:53060", local_ip_v4),
		timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	fst_check(session != NULL);
	if (session) {
		private_object_t *tech_pvt = switch_core_session_get_private(session);
		struct { switch_core_session_message_types_t id; int arg; } cases[] = {
			{ SWITCH_MESSAGE_RESAMPLE_EVENT, 1 },
			{ SWITCH_MESSAGE_RESAMPLE_EVENT, 0 },
			{ SWITCH_MESSAGE_INDICATE_TRANSCODING_NECESSARY, 0 }
		};
		unsigned int i;

		for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			media_notification_test_t test = { 0 };
			switch_threadattr_t *attr;
			switch_thread_t *worker;
			switch_status_t joined;
			switch_time_t deadline;
			int completed_while_locked = 0;
			int sofia_locked = 0;

			test.session = session;
			test.message_id = cases[i].id;
			test.numeric_arg = cases[i].arg;
			switch_mutex_init(&test.mutex, SWITCH_MUTEX_NESTED, fst_pool);
			switch_thread_cond_create(&test.cond, fst_pool);
			switch_threadattr_create(&attr, fst_pool);
			switch_threadattr_detach_set(attr, 0);
			switch_threadattr_stacksize_set(attr, SWITCH_THREAD_STACKSIZE);

			/* Wait for the writer to own its codec lock before taking the
			 * Sofia lock. Otherwise a pending SIP event can own the codec
			 * lock first and the test itself would invert the fixed order. */
			status = switch_thread_create(&worker, attr, send_media_notification, &test, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			if (status != SWITCH_STATUS_SUCCESS) {
				switch_thread_cond_destroy(test.cond);
				switch_mutex_destroy(test.mutex);
				continue;
			}
			switch_mutex_lock(test.mutex);
			while (!test.ready) {
				switch_thread_cond_wait(test.cond, test.mutex);
			}
			switch_mutex_unlock(test.mutex);

			/* A SIP dispatcher owns this mutex during codec setup. The
			 * notification must finish before it is released, otherwise
			 * codec setup's write-lock acquisition would close the cycle.
			 * Release it on timeout so the old code fails without hanging. */
			deadline = switch_micro_time_now() + 2000000;
			while (switch_micro_time_now() < deadline) {
				if (switch_mutex_trylock(tech_pvt->sofia_mutex) == SWITCH_STATUS_SUCCESS) {
					sofia_locked = 1;
					break;
				}
				switch_sleep(1000);
			}
			fst_check(sofia_locked);
			switch_mutex_lock(test.mutex);
			test.send = sofia_locked;
			test.proceed = 1;
			switch_thread_cond_signal(test.cond);
			if (sofia_locked) {
				deadline = switch_micro_time_now() + 2000000;
				while (!test.completed && switch_micro_time_now() < deadline) {
					switch_thread_cond_timedwait(test.cond, test.mutex, 100000);
				}
				completed_while_locked = test.completed;
			}
			switch_mutex_unlock(test.mutex);
			if (sofia_locked) {
				switch_mutex_unlock(tech_pvt->sofia_mutex);
			}
			switch_thread_join(&joined, worker);
			if (sofia_locked) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					"Notification %d/%d completed with Sofia locked: %d\n", cases[i].id, cases[i].arg, completed_while_locked);
				fst_check(completed_while_locked);
				fst_check(test.status == SWITCH_STATUS_SUCCESS);
			}
			switch_thread_cond_destroy(test.cond);
			switch_mutex_destroy(test.mutex);
		}
		switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1000000);
	}
}
FST_TEST_END()

#if HAVE_STIRSHAKEN
FST_TEST_BEGIN(sofia_verify_identity_test_no_identity)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{ignore_early_media=true}sofia/internal/verifyidentity@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status != SWITCH_STATUS_SUCCESS);
	fst_check(cause == SWITCH_CAUSE_NO_IDENTITY);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_verify_identity_test_bad_identity)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{ignore_early_media=true,sip_h_identity=foo;info=bar}sofia/internal/verifyidentity@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status != SWITCH_STATUS_SUCCESS);
	fst_check(cause == SWITCH_CAUSE_INVALID_IDENTITY);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_verify_identity_test_valid_identity_no_cert_available)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231234,ignore_early_media=true,sip_h_identity=eyJhbGciOiJFUzI1NiIsInBwdCI6InNoYWtlbiIsInR5cCI6InBhc3Nwb3J0IiwieDV1IjoiaHR0cDovLzEyNy4wLjAuMS80MDQucGVtIn0.eyJhdHRlc3QiOiJBIiwiZGVzdCI6eyJ0biI6WyIxNTU1MzIxNDMyMSJdfSwiaWF0IjoxNjE4Mjc5OTYzLCJvcmlnIjp7InRuIjoiMTU1NTEyMzEyMzQifSwib3JpZ2lkIjoiMTMxMzEzMTMifQ.Cm34sISkFWYB6ohtjjJEO71Hyz4TQ5qrTDyYmCXBj-ni5Fe7IbNjmMyvY_lD_Go0u2csWQNe8n03fHSO7Z7nNw;info=<http://127.0.0.1/404.pem>;alg=ES256;ppt=shaken}sofia/internal/+15553214321@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status != SWITCH_STATUS_SUCCESS);
	fst_check(cause == SWITCH_CAUSE_INVALID_IDENTITY);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_auth_identity_test_attest_a)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231234,ignore_early_media=true,sip_stir_shaken_attest=A}sofia/internal/+15553214322@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	fst_check(session);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_auth_identity_test_attest_b)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231234,ignore_early_media=true,sip_stir_shaken_attest=B}sofia/internal/+15553214322@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	fst_check(session);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_auth_identity_test_attest_c)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231234,ignore_early_media=true,sip_stir_shaken_attest=C}sofia/internal/+15553214322@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	fst_check(session);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_verify_identity_test_verified_attest_a_expired)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231234,ignore_early_media=true,sip_h_identity=eyJhbGciOiJFUzI1NiIsInBwdCI6InNoYWtlbiIsInR5cCI6InBhc3Nwb3J0IiwieDV1IjoiaHR0cDovLzEyNy4wLjAuMTo4MDgwL2NlcnQucGVtIn0.eyJhdHRlc3QiOiJBIiwiZGVzdCI6eyJ0biI6WyIxNTU1MzIxNDMyMiJdfSwiaWF0IjoxNjE4MzczMTc0LCJvcmlnIjp7InRuIjoiMTU1NTEyMzEyMzQifSwib3JpZ2lkIjoiMzliZDYzZDQtOTE1Mi00MzU0LWFkNjctNjg5NjQ2NmI4ZDI3In0.mUaikwHSOb8RVPwwMZTsqBe57MZY29CgbIqmiiEmyq9DzKZO-y4qShiIVT3serg-xHgC9SCMjUOBWaDfeXnEvA;info=<http://127.0.0.1:8080/cert.pem>;alg=ES256;ppt=shaken}sofia/internal/+15553214322@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status != SWITCH_STATUS_SUCCESS);
	fst_check(cause == SWITCH_CAUSE_CALL_REJECTED);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_auth_identity_test_attest_a_date)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231235,ignore_early_media=true,sip_stir_shaken_attest=A}sofia/internal/+15553214323@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	fst_check(session);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(10 * 1000 * 1000);
	}
}
FST_TEST_END()
#endif 

FST_MODULE_END()

FST_CORE_END()


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
