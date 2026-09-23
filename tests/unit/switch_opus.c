#include <switch.h>
#include <test/switch_test.h>

/* Use the null endpoint for the session lifecycle, and the real media core for
 * SDP negotiation and RTP writes. No SIP server or external peer is needed. */
static switch_status_t opus_test_session(const char *codecs, switch_core_session_t **session)
{
	switch_call_cause_t cause;
	switch_media_handle_t *handle;
	switch_core_media_params_t *params;
	switch_channel_t *channel;
	switch_status_t status;

	status = switch_ivr_originate(NULL, session, &cause, "null/opus-test", 2,
			NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	if (status != SWITCH_STATUS_SUCCESS || !*session) return SWITCH_STATUS_FALSE;
	channel = switch_core_session_get_channel(*session);
	params = switch_core_session_alloc(*session, sizeof(*params));
	params->inbound_codec_string = switch_core_session_strdup(*session, codecs);
	params->outbound_codec_string = switch_core_session_strdup(*session, codecs);
	params->rtpip = switch_core_session_strdup(*session, "127.0.0.1");
	status = switch_media_handle_create(&handle, *session, params);
	if (status != SWITCH_STATUS_SUCCESS) return status;
	switch_channel_set_variable(channel, "absolute_codec_string", codecs);
	switch_channel_set_variable(channel, "rtp_codec_negotiation", "generous");
	switch_channel_set_variable(channel, "send_silence_when_idle", "-1");
	switch_channel_set_variable(channel, SWITCH_LOCAL_MEDIA_IP_VARIABLE, "127.0.0.1");
	switch_core_media_prepare_codecs(*session, SWITCH_FALSE);
	return SWITCH_STATUS_SUCCESS;
}

static void opus_test_close(switch_core_session_t **session)
{
	if (*session) {
		switch_core_media_deactivate_rtp(*session);
		switch_channel_hangup(switch_core_session_get_channel(*session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(*session);
		*session = NULL;
	}
}

static char *opus_test_sdp(switch_core_session_t *session, int rate, const char *fmtp, int port)
{
	return switch_core_session_sprintf(session,
			"v=0\r\no=test 1 1 IN IP4 127.0.0.1\r\ns=test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
			"m=audio %d RTP/AVP 111 0\r\na=rtpmap:111 opus/%d/2\r\n%s%s%s"
			"a=rtpmap:0 PCMU/8000\r\na=ptime:20\r\n",
			port, rate, *fmtp ? "a=fmtp:111 " : "", fmtp, *fmtp ? "\r\n" : "");
}

static switch_status_t opus_test_write_rtp(switch_core_session_t *session, switch_frame_t *frame,
			switch_io_flag_t flags, int stream_id)
{
	return switch_core_media_write_frame(session, frame, flags, stream_id, SWITCH_MEDIA_TYPE_AUDIO);
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_opus)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_opus");
		}
		FST_SETUP_END()
		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(test_opus_negotiation)
		{
			/* PCMU is also offered so a near-match cannot silently pass this test.
			 * The selected PCM rate must remain within the local preference list. */
			struct {
				const char *codecs;
				int rtp_rate;
				const char *fmtp;
				int expected_rate;
			} cases[] = {
				{ "opus@16000h,PCMU", 48000, "", 16000 },
				{ "opus@8000h,PCMU", 48000, "", 8000 },
				{ "opus@16000h,PCMU", 48000, "useinbandfec=1", 16000 },
				{ "opus@16000h,PCMU", 48000, "sprop-maxcapturerate=16000", 16000 },
				{ "opus@8000h,PCMU", 48000, "sprop-maxcapturerate=8000", 8000 },
				{ "opus@16000h,PCMU", 48000, "maxplaybackrate=16000", 16000 },
				{ "opus@48000h", 48000, "maxplaybackrate=16000", 48000 },
				{ "opus@48000h", 48000, "sprop-maxcapturerate=16000", 48000 },
				{ "opus@48000h,PCMU", 48000, "maxplaybackrate=16000", 48000 },
				{ "opus@48000h,PCMU", 48000, "sprop-maxcapturerate=16000", 48000 },
				{ "opus@16000h,PCMU", 16000, "", 16000 },
				{ "opus@8000h,PCMU", 8000, "", 8000 },
				{ "opus@48000h,opus@16000h,PCMU", 16000, "", 16000 },
				{ "opus@16000h,PCMU", 48000, "sprop-maxcapturerate=48000", 16000 },
				{ "opus@16000h,PCMU", 48000, "maxplaybackrate=48000", 16000 },
				{ "opus@48000h,opus@16000h,PCMU", 48000, "maxplaybackrate=16000", 16000 },
				{ "opus@16000h,opus@48000h,PCMU", 48000, "maxplaybackrate=16000", 48000 },
				{ "opus@48000h,PCMU", 48000, "maxplaybackrate=16000;sprop-maxcapturerate=8000", 48000 },
				{ "opus@48000h,PCMU", 48000, "sprop-maxcapturerate=8000;maxplaybackrate=16000", 48000 },
				{ "opus@16000h,PCMU", 48000, "maxplaybackrate=16000;sprop-maxcapturerate=48000", 16000 },
				{ "opus@16000h,PCMU", 48000, "sprop-maxcapturerate=48000;maxplaybackrate=16000", 16000 },
				{ "PCMU", 48000, "sprop-maxcapturerate=16000", 8000 }
			};
			unsigned int i;
			for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
				switch_core_session_t *session = NULL;
				switch_codec_implementation_t impl = { 0 };
				uint8_t proceed = 0, match;
				fst_requires(opus_test_session(cases[i].codecs, &session) == SWITCH_STATUS_SUCCESS);
				match = switch_core_media_negotiate_sdp(session,
						opus_test_sdp(session, cases[i].rtp_rate, cases[i].fmtp, 54320), &proceed, SDP_OFFER);
				fst_xcheck(match == 1, switch_core_sprintf(fst_pool, "case %u: %s / opus/%d / %s", i, cases[i].codecs, cases[i].rtp_rate, cases[i].fmtp));
				if (match) {
					switch_core_session_get_read_impl(session, &impl);
					fst_xcheck(impl.actual_samples_per_second == cases[i].expected_rate,
							switch_core_sprintf(fst_pool, "case %u: expected PCM rate %d, got %u", i, cases[i].expected_rate, impl.actual_samples_per_second));
					fst_check(!strcasecmp(impl.iananame, !strcmp(cases[i].codecs, "PCMU") ? "PCMU" : "opus"));
				}
				opus_test_close(&session);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_non_opus_negotiation)
		{
			struct {
				const char *codecs;
				const char *encoding;
				int payload;
				int rate;
				const char *expected;
			} cases[] = {
				{ "G722,PCMU", "G722", 9, 8000, "G722" },
				{ "G722,PCMU", "G722", 9, 16000, "PCMU" },
				{ "PCMA,PCMU", "PCMA", 8, 8000, "PCMA" },
				{ "PCMA,PCMU", "PCMA", 8, 16000, "PCMU" }
			};
			unsigned int i;
			fst_requires_module("mod_spandsp");
			for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
				switch_core_session_t *session = NULL;
				switch_codec_implementation_t impl = { 0 };
				uint8_t proceed = 0;
				char *sdp;
				fst_requires(opus_test_session(cases[i].codecs, &session) == SWITCH_STATUS_SUCCESS);
				sdp = switch_core_session_sprintf(session,
						"v=0\r\no=test 1 1 IN IP4 127.0.0.1\r\ns=test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
						"m=audio 54320 RTP/AVP %d 0\r\na=rtpmap:%d %s/%d\r\na=rtpmap:0 PCMU/8000\r\na=ptime:20\r\n",
						cases[i].payload, cases[i].payload, cases[i].encoding, cases[i].rate);
				fst_check(switch_core_media_negotiate_sdp(session, sdp, &proceed, SDP_OFFER) == 1);
				switch_core_session_get_read_impl(session, &impl);
				fst_check(impl.iananame && !strcasecmp(impl.iananame, cases[i].expected));
				opus_test_close(&session);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_opus_rtp_clock)
		{
			int rates[] = { 8000, 16000, 48000 };
			unsigned int r;
			for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
				switch_core_session_t *session = NULL;
				switch_socket_t *receiver = NULL;
				switch_sockaddr_t *addr;
				switch_codec_t pcm = { 0 };
				char codecs[32];
				char *sdp;
				uint8_t proceed = 0;
				int phase, transcode, packet;
				int16_t samples[960] = { 0 };
				unsigned char silence[] = { 0xf8, 0xff, 0xfe };

				fst_requires(switch_sockaddr_info_get(&addr, "127.0.0.1", SWITCH_INET, 0, 0, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_socket_create(&receiver, SWITCH_INET, SOCK_DGRAM, SWITCH_PROTO_UDP, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_socket_bind(receiver, addr) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_socket_addr_get(&addr, SWITCH_FALSE, receiver) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_socket_timeout_set(receiver, 1000000) == SWITCH_STATUS_SUCCESS);
				switch_snprintf(codecs, sizeof(codecs), "opus@%dh", rates[r]);
				fst_requires(opus_test_session(codecs, &session) == SWITCH_STATUS_SUCCESS);
				sdp = opus_test_sdp(session, 48000, "", switch_sockaddr_get_port(addr));
				fst_requires(switch_core_media_negotiate_sdp(session, sdp, &proceed, SDP_OFFER) == 1);
				fst_requires(switch_core_media_choose_ports(session, SWITCH_TRUE, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_media_activate_rtp(session) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_event_hook_add_write_frame(session, opus_test_write_rtp) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_codec_init(&pcm, "L16", NULL, NULL, rates[r], 20, 1,
						SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, fst_pool) == SWITCH_STATUS_SUCCESS);

				for (phase = 0; phase < 2; phase++) {
					if (phase) {
						/* Exercise the post-negotiation interval update on an active RTP session. */
						fst_requires(switch_core_media_negotiate_sdp(session, sdp, &proceed, SDP_OFFER) == 1);
						fst_requires(switch_core_media_set_codec(session, 1, 0) == SWITCH_STATUS_SUCCESS);
					}
					for (transcode = 0; transcode < 2; transcode++) {
						uint32_t previous = 0;
						for (packet = 0; packet < 4; packet++) {
							switch_frame_t frame = { 0 };
							char received[2048];
							switch_size_t len = sizeof(received);
							uint32_t timestamp;
							frame.codec = transcode ? &pcm : NULL;
							frame.data = transcode ? (void *) samples : (void *) silence;
							frame.datalen = frame.buflen = transcode ? rates[r] / 50 * sizeof(int16_t) : sizeof(silence);
							frame.samples = rates[r] / 50;
							frame.rate = rates[r];
							frame.channels = 1;
							frame.timestamp = transcode ? 100000 + packet * rates[r] / 50 : 0;
							if (transcode) {
								fst_requires(switch_core_session_write_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0) == SWITCH_STATUS_SUCCESS);
							} else {
								fst_requires(switch_core_media_write_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0, SWITCH_MEDIA_TYPE_AUDIO) == SWITCH_STATUS_SUCCESS);
							}
							fst_requires(switch_socket_recvfrom(addr, receiver, 0, received, &len) == SWITCH_STATUS_SUCCESS);
							fst_requires(len >= 12);
							fst_check_int_equals(received[1] & 0x7f, 111);
							memcpy(&timestamp, received + 4, sizeof(timestamp));
							timestamp = ntohl(timestamp);
							if (packet) {
								fst_xcheck((uint32_t)(timestamp - previous) == 960,
									switch_core_sprintf(fst_pool, "PCM %d, renegotiated %d, transcode %d: RTP delta %u, expected 960",
									rates[r], phase, transcode, timestamp - previous));
							}
							previous = timestamp;
						}
					}
				}
				switch_core_codec_destroy(&pcm);
				opus_test_close(&session);
				switch_socket_close(receiver);
			}
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
