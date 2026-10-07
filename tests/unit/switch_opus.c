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

/* Plain UDP socket on an ephemeral loopback port, standing in for the remote RTP peer. */
static switch_status_t rtp_test_peer(switch_memory_pool_t *pool, switch_socket_t **peer, switch_sockaddr_t **peer_addr)
{
	if (switch_sockaddr_info_get(peer_addr, "127.0.0.1", SWITCH_INET, 0, 0, pool) != SWITCH_STATUS_SUCCESS ||
		switch_socket_create(peer, SWITCH_INET, SOCK_DGRAM, SWITCH_PROTO_UDP, pool) != SWITCH_STATUS_SUCCESS ||
		switch_socket_bind(*peer, *peer_addr) != SWITCH_STATUS_SUCCESS ||
		switch_socket_addr_get(peer_addr, SWITCH_FALSE, *peer) != SWITCH_STATUS_SUCCESS ||
		switch_socket_timeout_set(*peer, 5000) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	return SWITCH_STATUS_SUCCESS;
}

/* Fixed 12-byte RTP header; byte 1 carries the marker bit and payload type. */
static void rtp_test_header(unsigned char *packet, uint8_t m_pt, uint16_t seq, uint32_t ts)
{
	uint32_t ssrc = htonl(0x12345678);

	packet[0] = 0x80;
	packet[1] = m_pt;
	packet[2] = seq >> 8;
	packet[3] = seq & 0xff;
	ts = htonl(ts);
	memcpy(packet + 4, &ts, 4);
	memcpy(packet + 8, &ssrc, 4);
}

/* Loopback RTP session paired with a plain UDP peer socket, for RFC 4733 checks. */
static switch_rtp_t *dtmf_test_rtp(switch_memory_pool_t *pool, uint32_t samples_per_interval,
			switch_socket_t **peer, switch_sockaddr_t **rtp_addr)
{
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	switch_sockaddr_t *peer_addr;
	switch_port_t rtp_port = switch_rtp_request_port("127.0.0.1");
	const char *err = NULL;
	switch_rtp_t *rtp;

	if (rtp_test_peer(pool, peer, &peer_addr) != SWITCH_STATUS_SUCCESS ||
		switch_sockaddr_info_get(rtp_addr, "127.0.0.1", SWITCH_INET, rtp_port, 0, pool) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}

	flags[SWITCH_RTP_FLAG_USE_TIMER] = 1;
	rtp = switch_rtp_new("127.0.0.1", rtp_port, "127.0.0.1", switch_sockaddr_get_port(peer_addr), 111,
			samples_per_interval, 20000, flags, "soft", &err, pool, 0, 0);
	if (rtp) {
		switch_rtp_set_telephony_event(rtp, 101);
		switch_rtp_set_telephony_recv_event(rtp, 101);
	}
	return rtp;
}

static void dtmf_test_read(switch_rtp_t *rtp)
{
	switch_frame_t frame = { 0 };
	switch_rtp_zerocopy_read_frame(rtp, &frame, SWITCH_IO_FLAG_NONE);
}

static void dtmf_test_send_event(switch_socket_t *peer, switch_sockaddr_t *rtp_addr, uint16_t seq, uint32_t ts,
			int marker, int end, uint16_t duration)
{
	unsigned char packet[16];
	switch_size_t len = sizeof(packet);

	rtp_test_header(packet, (marker ? 0x80 : 0) | 101, seq, ts);
	packet[12] = 5; /* digit '5' */
	packet[13] = (end ? 0x80 : 0) | 10;
	packet[14] = duration >> 8;
	packet[15] = duration & 0xff;
	switch_socket_sendto(peer, rtp_addr, 0, (void *) packet, &len);
}

static switch_status_t opus_test_write_rtp(switch_core_session_t *session, switch_frame_t *frame,
			switch_io_flag_t flags, int stream_id)
{
	return switch_core_media_write_frame(session, frame, flags, stream_id, SWITCH_MEDIA_TYPE_AUDIO);
}

/* Synthetic audio only: exercise the real UDP/RTP/media read path without
 * embedding a customer's capture or requiring a licensed G.729 decoder. */
static switch_status_t opus_test_send_audio(switch_socket_t *peer, switch_sockaddr_t *rtp_addr,
		uint8_t payload, uint16_t seq, uint32_t timestamp)
{
	unsigned char packet[172];
	/* A 20 ms Opus frame padded to 40 bytes, a size the CBR timing check also inspects. */
	unsigned char opus_silence[40] = { 0xfb, 0x41, 0x23, 0xff, 0xfe };
	switch_size_t len = 12;

	rtp_test_header(packet, payload, seq, timestamp);
	if (payload == 111) {
		memcpy(packet + 12, opus_silence, sizeof(opus_silence));
		len += sizeof(opus_silence);
	} else {
		len += payload == 18 ? 20 : 160;
		memset(packet + 12, payload == 18 ? 0 : 0xff, len - 12);
	}
	return switch_socket_sendto(peer, rtp_addr, 0, (void *) packet, &len);
}

/* Feed the real read/media-bug/write path the frame produced by a jitter
 * buffer. A PLC frame may still contain stale DTMF/CNG bytes, not Opus audio. */
typedef struct {
	switch_frame_t input;
	unsigned int recorded_bytes;
	unsigned int replacements;
	unsigned int replacement_plc;
} opus_plc_test_t;

static switch_status_t opus_plc_read(switch_core_session_t *session, switch_frame_t **frame,
			switch_io_flag_t flags, int stream_id)
{
	opus_plc_test_t *test = switch_channel_get_private(switch_core_session_get_channel(session), "opus-plc-test");
	*frame = &test->input;
	return SWITCH_STATUS_SUCCESS;
}

static switch_bool_t opus_plc_bug(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	opus_plc_test_t *test = user_data;

	if (type == SWITCH_ABC_TYPE_READ) {
		unsigned char data[SWITCH_RECOMMENDED_BUFFER_SIZE];
		switch_frame_t frame = { 0 };
		frame.data = data;
		frame.buflen = sizeof(data);
		if (switch_core_media_bug_read(bug, &frame, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
			test->recorded_bytes += frame.datalen;
		}
	} else if (type == SWITCH_ABC_TYPE_READ_REPLACE) {
		switch_frame_t *frame = switch_core_media_bug_get_read_replace_frame(bug);
		test->replacements++;
		if (switch_test_flag(frame, SFF_PLC)) test->replacement_plc++;
		switch_core_media_bug_set_read_replace_frame(bug, frame);
	}
	return SWITCH_TRUE;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_opus)
	{
		FST_SETUP_BEGIN()
		{
			/* These in-process cases originate more sessions than the default
			 * production rate limit permits within one clock tick. */
			switch_core_sessions_per_second(1000);
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_opus");
		}
		FST_SETUP_END()
		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(test_negotiated_payload_switch)
		{
			/* The jitter buffer delays frames, so that case checks each frame
			 * against its own payload rather than the packet just sent. */
			struct { uint8_t alternate; const char *jb_msec; } cases[] = { { 0, NULL }, { 18, NULL }, { 0, "60" } };
			unsigned int i;
			for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
				switch_core_session_t *session = NULL, *sink = NULL;
				switch_codec_t pcmu = { 0 };
				switch_call_cause_t cause;
				switch_socket_t *peer = NULL;
				switch_sockaddr_t *peer_addr, *rtp_addr;
				switch_channel_t *channel;
				switch_rtp_t *rtp;
				uint8_t proceed = 0;
				uint16_t seq = 100;
				uint32_t timestamp = 1000;
				uint8_t last_payload = 111;
				int phase, packet;
				char *sdp;

				if (cases[i].alternate == 18 && switch_loadable_module_exists("mod_g729") != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "mod_g729 is not loaded, skipping the G.729 case\n");
					continue;
				}

				fst_requires(rtp_test_peer(fst_pool, &peer, &peer_addr) == SWITCH_STATUS_SUCCESS);
				fst_requires(opus_test_session("opus@16000h,PCMU,G729", &session) == SWITCH_STATUS_SUCCESS);
				/* Sofia profiles enable timing correction by default. */
				switch_media_handle_set_media_flag(switch_core_session_get_media_handle(session), SCMF_AUTOFIX_TIMING);
				channel = switch_core_session_get_channel(session);
				sdp = switch_core_session_sprintf(session,
						"v=0\r\no=test 1 1 IN IP4 127.0.0.1\r\ns=test\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
						"m=audio %d RTP/AVP 111 0 18\r\na=rtpmap:111 opus/48000/2\r\n"
						"a=rtpmap:0 PCMU/8000\r\na=rtpmap:18 G729/8000\r\na=ptime:20\r\n",
						switch_sockaddr_get_port(peer_addr));
				fst_requires(switch_core_media_negotiate_sdp(session, sdp, &proceed, SDP_OFFER) == 1);
				fst_requires(switch_core_media_choose_ports(session, SWITCH_TRUE, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
				if (cases[i].jb_msec) {
					switch_channel_set_variable(channel, "jitterbuffer_msec", cases[i].jb_msec);
				}
				fst_requires(switch_core_media_activate_rtp(session) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_sockaddr_info_get(&rtp_addr, "127.0.0.1", SWITCH_INET,
						atoi(switch_channel_get_variable(channel, SWITCH_LOCAL_MEDIA_PORT_VARIABLE)), 0, fst_pool) == SWITCH_STATUS_SUCCESS);
				rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
				fst_requires(rtp != NULL);
				fst_requires(!cases[i].jb_msec == !switch_rtp_get_jitter_buffer(rtp));
				switch_rtp_clear_flag(rtp, SWITCH_RTP_FLAG_PAUSE);
				if (cases[i].alternate == 0 && !cases[i].jb_msec) {
					fst_requires(switch_ivr_originate(NULL, &sink, &cause, "null/payload-sink", 2,
							NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_core_codec_init(&pcmu, "PCMU", NULL, NULL, 8000, 20, 1,
							SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
							switch_core_session_get_pool(sink)) == SWITCH_STATUS_SUCCESS);
					switch_core_session_set_write_codec(sink, &pcmu);
				}

				/* Phases 0-4: both directions, repeated, with steady traffic between
				 * changes. Phase 5: a payload type that was never negotiated, let
				 * through by the RTP layer (its PT filter also guards the jitter
				 * buffer, so that case skips it). Phase 6: a 4-packet burst of the
				 * alternate, then Opus, as a Yealink sends while resuming from hold.
				 * A packet may come back as CNG; every audio frame must already carry
				 * the decoder matching its received payload, including the first. */
				for (phase = 0; phase < 7; phase++) {
					int unknown = phase == 5, burst = phase == 6;
					int packets = burst ? 16 : unknown ? 3 : 12;
					int audio_frames = 0, opus_frames = 0, unknown_cng = 0;

					if (unknown) {
						if (cases[i].jb_msec) continue;
						switch_rtp_intentional_bugs(rtp, RTP_BUG_ACCEPT_ANY_PACKETS);
					}

					for (packet = 0; packet < packets; packet++) {
						uint8_t payload = unknown ? 8 : burst ? (packet < 4 ? cases[i].alternate : 111) :
								(phase % 2 ? cases[i].alternate : 111);
						const char *expected, *iananame;
						switch_frame_t *frame = NULL;

						if (payload == 111 && last_payload != 111) {
							/* The Yealink in the capture restarts the Opus clock when it switches back. */
							timestamp = 960;
						}
						last_payload = payload;
						fst_requires(opus_test_send_audio(peer, rtp_addr, payload, seq++, timestamp) == SWITCH_STATUS_SUCCESS);
						timestamp += payload == 111 ? 960 : 160;
						fst_requires(switch_core_media_read_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0,
								SWITCH_MEDIA_TYPE_AUDIO) == SWITCH_STATUS_SUCCESS);
						fst_requires(frame != NULL);
						/* A stale Opus decoder given the synthetic PCMU packet
						 * also reproduces the fatal write error seen by a bridge. */
						if (sink) {
							fst_check(switch_core_session_write_frame(sink, frame, SWITCH_IO_FLAG_NONE, 0) == SWITCH_STATUS_SUCCESS);
						}
						if (switch_test_flag(frame, SFF_CNG)) {
							unknown_cng += frame->payload == 8;
							continue;
						}
						fst_requires(frame->codec && frame->codec->implementation);
						iananame = frame->codec->implementation->iananame;
						expected = unknown ? "CNG" : frame->payload == 111 ? "OPUS" : frame->payload == 18 ? "G729" : "PCMU";
						fct_xchk((cases[i].jb_msec || frame->payload == payload) && !strcasecmp(iananame, expected),
								"case %u phase %d: payload %d returned with %s decoder, expected %s (sent payload %d)",
								i, phase, frame->payload, iananame, expected, payload);
						opus_frames += frame->payload == 111;
						audio_frames++;
					}
					if (unknown) {
						fct_xchk(unknown_cng > 0, "case %u: unnegotiated payload never reached the media layer", i);
					} else {
						fct_xchk(audio_frames > 0, "case %u phase %d: audio must recover after the codec change", i, phase);
					}
					if (burst && !cases[i].jb_msec) {
						/* The codec reset flushes queued RTP, which costs 2 packets in this
						 * send-one-read-one loop. Anything that delays the switch back loses more. */
						fct_xchk(opus_frames >= 10, "case %u: %d of 12 Opus packets after the burst were decoded", i, opus_frames);
					}
					fst_check(switch_channel_up_nosig(channel));
				}
				opus_test_close(&session);
				if (sink) {
					switch_core_session_unset_write_codec(sink);
					switch_core_codec_destroy(&pcmu);
					switch_channel_hangup(switch_core_session_get_channel(sink), SWITCH_CAUSE_NORMAL_CLEARING);
					switch_core_session_rwunlock(sink);
				}
				switch_socket_close(peer);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_opus_plc_recording_transcode)
		{
			int mode;
			/* No bug, recording, then recording plus a read-replace callback. */
			for (mode = 0; mode < 3; mode++) {
				switch_core_session_t *source = NULL, *sink = NULL;
				switch_codec_t opus = { 0 }, pcmu = { 0 };
				switch_call_cause_t cause;
				switch_media_bug_t *bug = NULL;
				opus_plc_test_t test = { 0 };
				unsigned char stale[60] = { 0x41, 0x00, 0x16, 0x80 };
				unsigned char silence[] = { 0xf8, 0xff, 0xfe };
				int packet;

				fst_requires(switch_ivr_originate(NULL, &source, &cause, "null/plc-source", 2,
						NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_ivr_originate(NULL, &sink, &cause, "null/plc-sink", 2,
						NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_codec_init(&opus, "OPUS", "mod_opus", NULL, 48000, 20, 1,
						SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
						switch_core_session_get_pool(source)) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_codec_init(&pcmu, "PCMU", NULL, NULL, 8000, 20, 1,
						SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
						switch_core_session_get_pool(sink)) == SWITCH_STATUS_SUCCESS);
				switch_core_session_set_read_codec(source, &opus);
				switch_core_session_set_write_codec(sink, &pcmu);
				switch_channel_set_private(switch_core_session_get_channel(source), "opus-plc-test", &test);
				fst_requires(switch_core_event_hook_add_read_frame(source, opus_plc_read) == SWITCH_STATUS_SUCCESS);
				if (mode) {
					fst_requires(switch_core_media_bug_add(source, "plc-recording", NULL, opus_plc_bug, &test, 0,
							SMBF_READ_STREAM | (mode == 2 ? SMBF_READ_REPLACE : 0), &bug) == SWITCH_STATUS_SUCCESS);
				}

				/* Normal audio, three consecutive concealed frames, then recovery. */
				for (packet = 0; packet < 5; packet++) {
					switch_frame_t *frame = NULL;
					int plc = packet > 0 && packet < 4;
					memset(&test.input, 0, sizeof(test.input));
					test.input.data = plc ? stale : silence;
					test.input.datalen = test.input.buflen = plc ? sizeof(stale) : sizeof(silence);
					test.input.codec = &opus;
					test.input.rate = 48000;
					test.input.samples = 960;
					test.input.channels = 1;
					test.input.flags = plc ? SFF_PLC : 0;
					test.input.seq = 28942 + packet;
					test.input.payload = 103;
					fst_requires(switch_core_session_read_frame(source, &frame, SWITCH_IO_FLAG_NONE, 0) == SWITCH_STATUS_SUCCESS);
					fst_requires(frame != NULL);
					if (mode != 2) {
						fst_check(frame == &test.input);
						fst_xcheck(!!switch_test_flag(frame, SFF_PLC) == plc,
								switch_core_sprintf(fst_pool, "mode %d packet %d: original PLC flag must survive recording", mode, packet));
					}
					fst_xcheck(switch_core_session_write_frame(sink, frame, SWITCH_IO_FLAG_NONE, 0) == SWITCH_STATUS_SUCCESS,
							switch_core_sprintf(fst_pool, "mode %d packet %d: bridge transcode must succeed", mode, packet));
				}
				if (mode) {
					fst_check_int_equals(test.recorded_bytes, 5 * 960 * sizeof(int16_t));
					fst_check_int_equals(test.replacements, mode == 2 ? 5 : 0);
					fst_check_int_equals(test.replacement_plc, 0);
					switch_core_media_bug_remove(source, &bug);
				}
				switch_core_event_hook_remove_read_frame(source, opus_plc_read);
				switch_channel_set_private(switch_core_session_get_channel(source), "opus-plc-test", NULL);
				switch_core_session_set_read_codec(source, NULL);
				switch_core_session_unset_write_codec(sink);
				switch_core_codec_destroy(&opus);
				switch_core_codec_destroy(&pcmu);
				opus_test_close(&source);
				opus_test_close(&sink);
			}
		}
		FST_TEST_END()

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

		FST_TEST_BEGIN(test_rfc2833_rtp_clock)
		{
			/* switch_dtmf_t.duration is in 8 kHz samples; RFC 4733 durations on the wire
			 * run at the RTP clock (48 kHz for Opus). A 250 ms digit must span ~250 ms of
			 * RTP clock when sent, and a 200 ms received event must be 1600 core samples. */
			uint32_t intervals[] = { 160, 960 };
			unsigned int r;
			for (r = 0; r < sizeof(intervals) / sizeof(intervals[0]); r++) {
				uint32_t spi = intervals[r], rate = spi * 50;
				switch_core_session_t *session = NULL;
				switch_call_cause_t cause;
				switch_socket_t *peer = NULL;
				switch_sockaddr_t *rtp_addr = NULL;
				switch_rtp_t *rtp;
				switch_dtmf_t dtmf = { '5', 2000, 0, 0 };
				uint32_t last_duration = 0, packets = 0, expected_rx = 1600;
				int loops, end = 0;
				uint16_t seq = 100;

				/* The RTP session finds its core session through the memory pool. */
				fst_requires(switch_ivr_originate(NULL, &session, &cause, "null/dtmf-test", 2,
						NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
				rtp = dtmf_test_rtp(switch_core_session_get_pool(session), spi, &peer, &rtp_addr);
				fst_requires(rtp != NULL);

				fst_requires(switch_rtp_queue_rfc2833(rtp, &dtmf) == SWITCH_STATUS_SUCCESS);
				for (loops = 0; loops < 60 && !end; loops++) {
					unsigned char received[256];
					switch_size_t len = sizeof(received);
					switch_sockaddr_t *from;

					dtmf_test_read(rtp);
					switch_sockaddr_info_get(&from, "127.0.0.1", SWITCH_INET, 0, 0, fst_pool);
					while (!end && switch_socket_recvfrom(from, peer, 0, (char *) received, &len) == SWITCH_STATUS_SUCCESS && len) {
						if (len >= 16 && (received[1] & 0x7f) == 101) {
							uint32_t duration = (received[14] << 8) | received[15];
							if (packets && !(received[13] & 0x80)) {
								fst_xcheck(duration - last_duration == spi,
									switch_core_sprintf(fst_pool, "%u Hz: duration step %u, expected %u", rate, duration - last_duration, spi));
							}
							last_duration = duration;
							end = received[13] & 0x80;
							packets++;
						}
						len = sizeof(received);
					}
				}
				fst_xcheck(end && last_duration >= rate / 4 && last_duration < rate / 4 + spi,
					switch_core_sprintf(fst_pool, "%u Hz: 250 ms digit ended at duration %u, expected %u", rate, last_duration, rate / 4));

				/* 200 ms event from the peer, durations stepping at the RTP clock */
				for (loops = 1; loops <= 10; loops++) {
					dtmf_test_send_event(peer, rtp_addr, seq++, 5000, loops == 1, 0, (uint16_t) (loops * spi));
				}
				for (loops = 0; loops < 3; loops++) {
					dtmf_test_send_event(peer, rtp_addr, seq++, 5000, 0, 1, (uint16_t) (10 * spi));
				}
				for (loops = 0; loops < 50 && !switch_rtp_has_dtmf(rtp); loops++) {
					dtmf_test_read(rtp);
				}
				memset(&dtmf, 0, sizeof(dtmf));
				fst_check(switch_rtp_dequeue_dtmf(rtp, &dtmf) == 1);
				fst_check(dtmf.digit == '5');
				fst_xcheck(dtmf.duration == expected_rx,
					switch_core_sprintf(fst_pool, "%u Hz: received 200 ms digit as %u core samples, expected %u", rate, dtmf.duration, expected_rx));

				switch_rtp_destroy(&rtp);
				switch_socket_close(peer);
				switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(session);
			}
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
