#include <tinytest.h>
#include "turbo_rtsp_parser.h"
#include "turbo_rtsp_rtp.h"
#include "turbo_rtsp_sdp.h"

#ifdef TURBO_MEDIA_HAS_RTSP

#include <stdio.h>
#include <string.h>

static void turbo_rtsp_test_rtp_payload_header(
    turbo_rtsp_rtp_header_t *header,
    uint16_t sequence_number,
    uint32_t timestamp,
    uint32_t ssrc,
    const uint8_t *payload,
    size_t payload_len) {
  memset(header, 0, sizeof(*header));
  header->version = 2;
  header->payload_type = 96;
  header->sequence_number = sequence_number;
  header->timestamp = timestamp;
  header->ssrc = ssrc;
  header->payload = payload;
  header->payload_len = payload_len;
}

static size_t turbo_rtsp_test_write_rtp_packet(
    uint8_t *packet,
    size_t packet_size,
    uint16_t sequence_number,
    uint32_t timestamp,
    uint32_t ssrc,
    const uint8_t *payload,
    size_t payload_len) {
  turbo_rtsp_rtp_header_t header;
  int written = 0;

  turbo_rtsp_test_rtp_payload_header(
      &header,
      sequence_number,
      timestamp,
      ssrc,
      payload,
      payload_len);
  written = turbo_rtsp_rtp_write_packet(
      packet,
      packet_size,
      &header,
      payload,
      payload_len);
  return written > 0 ? (size_t)written : 0;
}

static void turbo_rtsp_test_fill_mpeg2_ts(uint8_t *ts, size_t ts_len) {
  size_t offset = 0;

  memset(ts, 0, ts_len);
  for (offset = 0; offset + TURBO_RTSP_MPEG2_TS_PACKET_SIZE <= ts_len;
       offset += TURBO_RTSP_MPEG2_TS_PACKET_SIZE) {
    size_t index = offset / TURBO_RTSP_MPEG2_TS_PACKET_SIZE;
    ts[offset] = TURBO_RTSP_MPEG2_TS_SYNC_BYTE;
    ts[offset + 1] = (uint8_t)(0x40u | (index & 0x1fu));
    ts[offset + 2] = (uint8_t)index;
    ts[offset + 3] = 0x10;
  }
}

suite("turbo_rtsp_lib") {
  section("RFC 2326 RTSP messages") {
    it("parses a request line, CSeq, Session and stops at the first complete message") {
      const char *request =
          "DESCRIBE rtsp://example.com/media.mp4 RTSP/1.0\r\n"
          "CSeq: 312\r\n"
          "Accept: application/sdp\r\n"
          "Session: 12345678\r\n"
          "\r\n"
          "SETUP rtsp://example.com/media.mp4/trackID=0 RTSP/1.0\r\n";
      const size_t expected_consumed = strlen(
          "DESCRIBE rtsp://example.com/media.mp4 RTSP/1.0\r\n"
          "CSeq: 312\r\n"
          "Accept: application/sdp\r\n"
          "Session: 12345678\r\n"
          "\r\n");
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.method, TURBO_RTSP_METHOD_DESCRIBE);
      check_int_eq((int)parsed.cseq, 312);
      check_str_eq(parsed.uri, "rtsp://example.com/media.mp4");
      check_str_eq(parsed.session_id, "12345678");
      check_size_eq(consumed, expected_consumed);
    }

    it("reports partial input until the RTSP header terminator arrives") {
      const char *partial =
          "OPTIONS rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 1\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(partial, strlen(partial), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_PARTIAL);
      check_size_eq(consumed, 0);
    }

    it("consumes request bodies according to Content-Length before the next message") {
      const char *request =
          "SET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 4\r\n"
          "Content-Type: text/parameters\r\n"
          "Content-Length: 14\r\n"
          "\r\n"
          "volume: 0.75\r\n"
          "OPTIONS rtsp://example.com/live RTSP/1.0\r\n";
      const size_t expected_consumed = strlen(
          "SET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 4\r\n"
          "Content-Type: text/parameters\r\n"
          "Content-Length: 14\r\n"
          "\r\n"
          "volume: 0.75\r\n");
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.method, TURBO_RTSP_METHOD_SET_PARAMETER);
      check_int_eq((int)parsed.cseq, 4);
      check_size_eq(consumed, expected_consumed);
    }

    it("preserves headers and body as a zero-copy message view") {
      const char *request =
          "SET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 6\r\n"
          "Content-Type: text/parameters\r\n"
          "Content-Length: 14\r\n"
          "\r\n"
          "volume: 0.75\r\n";
      turbo_rtsp_message_t message;
      const turbo_rtsp_header_view_t *content_type = NULL;
      const turbo_rtsp_header_view_t *content_length = NULL;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_message(request, strlen(request), &consumed, &message);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(message.request.method, TURBO_RTSP_METHOD_SET_PARAMETER);
      check_int_eq((int)message.request.cseq, 6);
      check_size_eq(message.header_count, 3);
      content_type = turbo_rtsp_message_find_header(&message, "content-type");
      content_length = turbo_rtsp_message_find_header(&message, "Content-Length");
      check(content_type != NULL);
      check(content_length != NULL);
      check_mem_eq(content_type->value, "text/parameters", strlen("text/parameters"));
      check_mem_eq(content_length->value, "14", 2);
      check_size_eq(message.body_len, 14);
      check_mem_eq(message.body, "volume: 0.75\r\n", 14);
      check_size_eq(consumed, strlen(request));
    }

    it("reports partial input when Content-Length body is incomplete") {
      const char *request =
          "SET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 5\r\n"
          "Content-Length: 16\r\n"
          "\r\n"
          "volume";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_PARTIAL);
      check_size_eq(consumed, 0);
    }

    it("rejects non-RTSP request versions") {
      const char *invalid_version =
          "OPTIONS rtsp://example.com/live HTTP/1.1\r\n"
          "CSeq: 1\r\n"
          "\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(
          invalid_version,
          strlen(invalid_version),
          &consumed,
          &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_ERROR);
    }

    it("parses an OK response status line and headers") {
      const char *response =
          "RTSP/1.0 200 OK\r\n"
          "CSeq: 7\r\n"
          "Server: TurboMedia RTSP\r\n"
          "Content-Length: 0\r\n"
          "\r\n"
          "RTSP/1.0 404 Not Found\r\n";
      const size_t expected_consumed = strlen(
          "RTSP/1.0 200 OK\r\n"
          "CSeq: 7\r\n"
          "Server: TurboMedia RTSP\r\n"
          "Content-Length: 0\r\n"
          "\r\n");
      turbo_rtsp_response_view_t parsed;
      const turbo_rtsp_header_view_t *cseq = NULL;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_response(response, strlen(response), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.status_code, 200);
      check_mem_eq(parsed.reason, "OK", 2);
      check_size_eq(parsed.reason_len, 2);
      check_size_eq(parsed.header_count, 3);
      cseq = turbo_rtsp_response_find_header(&parsed, "cseq");
      check(cseq != NULL);
      check_mem_eq(cseq->value, "7", 1);
      check_size_eq(parsed.body_len, 0);
      check_size_eq(consumed, expected_consumed);
    }

    it("preserves response bodies as a zero-copy view") {
      const char *response =
          "RTSP/1.0 200 OK\r\n"
          "CSeq: 10\r\n"
          "Content-Type: application/sdp\r\n"
          "Content-Length: 10\r\n"
          "\r\n"
          "v=0\r\ns=x\r\n"
          "RTSP/1.0 200 OK\r\n";
      turbo_rtsp_response_view_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_response(response, strlen(response), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.status_code, 200);
      check_size_eq(parsed.body_len, 10);
      check_mem_eq(parsed.body, "v=0\r\ns=x\r\n", 10);
      check_size_eq(consumed, strlen(response) - strlen("RTSP/1.0 200 OK\r\n"));
    }

    it("reports partial response input when Content-Length body is incomplete") {
      const char *response =
          "RTSP/1.0 200 OK\r\n"
          "CSeq: 11\r\n"
          "Content-Length: 12\r\n"
          "\r\n"
          "v=0\r\n";
      turbo_rtsp_response_view_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_response(response, strlen(response), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_PARTIAL);
      check_size_eq(consumed, 0);
    }

    it("rejects response status lines with non-RTSP versions") {
      const char *response =
          "HTTP/1.1 200 OK\r\n"
          "CSeq: 12\r\n"
          "Content-Length: 0\r\n"
          "\r\n";
      turbo_rtsp_response_view_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_response(response, strlen(response), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_ERROR);
    }

    it("rejects duplicate conflicting response Content-Length headers") {
      const char *response =
          "RTSP/1.0 200 OK\r\n"
          "CSeq: 13\r\n"
          "Content-Length: 10\r\n"
          "Content-Length: 12\r\n"
          "\r\n"
          "v=0\r\ns=x\r\n";
      turbo_rtsp_response_view_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_response(response, strlen(response), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_ERROR);
    }

    it("formats an OPTIONS response with status line, CSeq, Server and Public") {
      turbo_rtsp_response_t response;
      char buffer[1024];
      int len = 0;

      memset(&response, 0, sizeof(response));
      response.status_code = 200;
      response.public_methods = "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN";

      len = turbo_rtsp_format_response(
          buffer,
          sizeof(buffer),
          7,
          "TurboMedia RTSP",
          &response);

      check(len > 0);
      check(strstr(buffer, "RTSP/1.0 200 OK\r\n") == buffer);
      check(strstr(buffer, "CSeq: 7\r\n") != NULL);
      check(strstr(buffer, "Server: TurboMedia RTSP\r\n") != NULL);
      check(strstr(buffer, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n") != NULL);
      check(strstr(buffer, "Content-Length: 0\r\n") != NULL);
      check(strstr(buffer, "\r\n\r\n") != NULL);
    }

    it("rejects duplicate conflicting Content-Length headers") {
      const char *request =
          "SET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 7\r\n"
          "Content-Length: 14\r\n"
          "Content-Length: 16\r\n"
          "\r\n"
          "volume: 0.75\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_ERROR);
    }

    it("rejects overflowing request CSeq and Content-Length values") {
      const char *overflow_cseq =
          "OPTIONS rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 4294967296\r\n"
          "\r\n";
      const char *overflow_content_length =
          "SET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 8\r\n"
          "Content-Length: 184467440737095516160\r\n"
          "\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;

      check_int_eq(
          turbo_rtsp_parse_request(
              overflow_cseq,
              strlen(overflow_cseq),
              &consumed,
              &parsed),
          TURBO_RTSP_PARSE_ERROR);
      check_int_eq(
          turbo_rtsp_parse_request(
              overflow_content_length,
              strlen(overflow_content_length),
              &consumed,
              &parsed),
          TURBO_RTSP_PARSE_ERROR);
    }

    it("rejects response headers managed by the formatter") {
      const turbo_rtsp_header_t headers[] = {
          {"Content-Length", "999"}
      };
      turbo_rtsp_response_t response;
      char buffer[256];

      memset(&response, 0, sizeof(response));
      response.status_code = 200;
      response.headers = headers;
      response.header_count = sizeof(headers) / sizeof(headers[0]);

      check_int_eq(
          turbo_rtsp_format_response(buffer, sizeof(buffer), 1, "TurboMedia RTSP", &response),
          -1);
    }

    it("parses Session timeout and NPT Range request headers") {
      const char *request =
          "PLAY rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 13\r\n"
          "Session: 12345678;timeout=45\r\n"
          "Range: npt=10.500-20.250\r\n"
          "\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_str_eq(parsed.session_id, "12345678");
      check_str_eq(parsed.session.id, "12345678");
      check_int_eq(parsed.session.has_timeout, 1);
      check_int_eq(parsed.session.timeout_seconds, 45);
      check_str_eq(parsed.range, "npt=10.500-20.250");
      check_int_eq(parsed.range_spec.type, TURBO_RTSP_RANGE_NPT);
      check_int_eq(parsed.range_spec.has_start, 1);
      check_int_eq((int)parsed.range_spec.start_ms, 10500);
      check_int_eq(parsed.range_spec.has_end, 1);
      check_int_eq((int)parsed.range_spec.end_ms, 20250);
    }

    it("parses open-ended NPT now ranges") {
      const char *request =
          "PLAY rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 14\r\n"
          "Range: npt=now-\r\n"
          "\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.range_spec.type, TURBO_RTSP_RANGE_NPT);
      check_int_eq(parsed.range_spec.has_start, 1);
      check_int_eq(parsed.range_spec.start_is_now, 1);
      check_int_eq(parsed.range_spec.has_end, 0);
    }

    it("parses NPT hh:mm:ss request ranges") {
      const char *request =
          "PLAY rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 15\r\n"
          "Range: npt=01:02:03.4-01:02:04.005\r\n"
          "\r\n";
      static turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = 0;

      memset(&parsed, 0, sizeof(parsed));
      rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.range_spec.type, TURBO_RTSP_RANGE_NPT);
      check_int_eq(parsed.range_spec.has_start, 1);
      check_int_eq((int)parsed.range_spec.start_ms, 3723400);
      check_int_eq(parsed.range_spec.has_end, 1);
      check_int_eq((int)parsed.range_spec.end_ms, 3724005);
    }

    it("parses SMPTE request ranges and absolute time parameters") {
      const char *request =
          "PLAY rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 16\r\n"
          "Range: smpte-25=10:07:00-10:07:33:05.01;time=19970123T153600Z\r\n"
          "\r\n";
      static turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = 0;

      memset(&parsed, 0, sizeof(parsed));
      rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_str_eq(parsed.range, "smpte-25=10:07:00-10:07:33:05.01;time=19970123T153600Z");
      check_int_eq(parsed.range_spec.type, TURBO_RTSP_RANGE_SMPTE_25);
      check_int_eq(parsed.range_spec.has_start, 1);
      check_int_eq((int)parsed.range_spec.start_ms, 36420000);
      check_int_eq(parsed.range_spec.has_end, 1);
      check_int_eq((int)parsed.range_spec.end_ms, 36453200);
      check_int_eq(parsed.range_spec.has_time, 1);
      check(parsed.range_spec.time_ms == 854033760000LL);
    }

    it("parses absolute clock request ranges") {
      const char *request =
          "PLAY rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 17\r\n"
          "Range: clock=19961108T143720.25Z-\r\n"
          "\r\n";
      const char *request_without_seconds =
          "PLAY rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 18\r\n"
          "Range: clock=19961110T1925-19961110T2015\r\n"
          "\r\n";
      static turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = 0;

      memset(&parsed, 0, sizeof(parsed));
      rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);
      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.range_spec.type, TURBO_RTSP_RANGE_CLOCK);
      check_int_eq(parsed.range_spec.has_start, 1);
      check(parsed.range_spec.start_ms == 847463840250LL);
      check_int_eq(parsed.range_spec.has_end, 0);

      memset(&parsed, 0, sizeof(parsed));
      consumed = 0;
      rc = turbo_rtsp_parse_request(
          request_without_seconds,
          strlen(request_without_seconds),
          &consumed,
          &parsed);
      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.range_spec.type, TURBO_RTSP_RANGE_CLOCK);
      check(parsed.range_spec.start_ms == 847653900000LL);
      check(parsed.range_spec.end_ms == 847656900000LL);
    }

    it("rejects malformed known Range request headers") {
      const char *bad_clock =
          "PLAY rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 19\r\n"
          "Range: clock=-19961108T143720.25Z\r\n"
          "\r\n";
      const char *bad_smpte =
          "PLAY rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 20\r\n"
          "Range: smpte-25=10:07:00:25-\r\n"
          "\r\n";
      const char *bad_npt =
          "PLAY rtsp://example.com/live RTSP/1.0\r\n"
          "CSeq: 21\r\n"
          "Range: npt=10-now\r\n"
          "\r\n";
      static turbo_rtsp_request_t parsed;
      size_t consumed = 0;

      memset(&parsed, 0, sizeof(parsed));
      check_int_eq(
          turbo_rtsp_parse_request(bad_clock, strlen(bad_clock), &consumed, &parsed),
          TURBO_RTSP_PARSE_ERROR);

      memset(&parsed, 0, sizeof(parsed));
      consumed = 0;
      check_int_eq(
          turbo_rtsp_parse_request(bad_smpte, strlen(bad_smpte), &consumed, &parsed),
          TURBO_RTSP_PARSE_ERROR);

      memset(&parsed, 0, sizeof(parsed));
      consumed = 0;
      check_int_eq(
          turbo_rtsp_parse_request(bad_npt, strlen(bad_npt), &consumed, &parsed),
          TURBO_RTSP_PARSE_ERROR);
    }

    it("formats RTP-Info as a managed response header") {
      const turbo_rtsp_header_t headers[] = {
          {"RTP-Info", "url=rtsp://example.com/live/trackID=0;seq=2;rtptime=3"}
      };
      turbo_rtsp_response_t response;
      char buffer[512];

      memset(&response, 0, sizeof(response));
      response.status_code = 200;
      response.rtp_info = "url=rtsp://example.com/live/trackID=0;seq=45102;rtptime=12345678";

      check(turbo_rtsp_format_response(buffer, sizeof(buffer), 15, "TurboMedia RTSP", &response) > 0);
      check(strstr(buffer, "RTP-Info: url=rtsp://example.com/live/trackID=0;seq=45102;rtptime=12345678\r\n") != NULL);

      response.rtp_info = NULL;
      response.headers = headers;
      response.header_count = sizeof(headers) / sizeof(headers[0]);
      check_int_eq(
          turbo_rtsp_format_response(buffer, sizeof(buffer), 15, "TurboMedia RTSP", &response),
          -1);
    }

    it("parses RTP-Info entries for PLAY responses") {
      turbo_rtsp_rtp_info_t infos[2];
      size_t info_count = 0;

      memset(infos, 0, sizeof(infos));

      check_int_eq(
          turbo_rtsp_parse_rtp_info(
              "url=rtsp://example.com/live/trackID=0;seq=45102;rtptime=12345678,"
              "url=rtsp://example.com/live/trackID=1;rtptime=42",
              0,
              infos,
              sizeof(infos) / sizeof(infos[0]),
              &info_count),
          0);
      check_size_eq(info_count, 2);
      check_str_eq(infos[0].url, "rtsp://example.com/live/trackID=0");
      check_int_eq(infos[0].has_seq, 1);
      check_int_eq((int)infos[0].seq, 45102);
      check_int_eq(infos[0].has_rtptime, 1);
      check_int_eq((int)infos[0].rtptime, 12345678);
      check_str_eq(infos[1].url, "rtsp://example.com/live/trackID=1");
      check_int_eq(infos[1].has_seq, 0);
      check_int_eq(infos[1].has_rtptime, 1);
      check_int_eq((int)infos[1].rtptime, 42);
    }

    it("rejects malformed RTP-Info entries") {
      turbo_rtsp_rtp_info_t info;
      size_t info_count = 0;

      memset(&info, 0, sizeof(info));

      check_int_eq(
          turbo_rtsp_parse_rtp_info(
              "seq=1;rtptime=2",
              0,
              &info,
              1,
              &info_count),
          -1);
      check_int_eq(
          turbo_rtsp_parse_rtp_info(
              "url=rtsp://example.com/live/trackID=0;seq=4294967296",
              0,
              &info,
              1,
              &info_count),
          -1);
    }
  }

  section("RFC 2326 RTSP client request formatting") {
    it("formats ANNOUNCE with SDP body length and client headers") {
      static const char sdp_body[] =
          "v=0\r\n"
          "o=- 2890844526 2890842807 IN IP4 192.0.2.1\r\n"
          "s=TurboMedia Push\r\n"
          "c=IN IP4 192.0.2.1\r\n"
          "t=0 0\r\n"
          "m=video 0 RTP/AVP 96\r\n"
          "a=rtpmap:96 H264/90000\r\n"
          "a=control:trackID=0\r\n";
      turbo_rtsp_request_message_t request;
      turbo_rtsp_message_t parsed;
      static char buffer[2048];
      static char content_length[64];
      const char *body = NULL;
      size_t consumed = 0;
      int len = 0;

      memset(buffer, 0, sizeof(buffer));
      memset(content_length, 0, sizeof(content_length));
      memset(&request, 0, sizeof(request));
      request.method = TURBO_RTSP_METHOD_ANNOUNCE;
      request.uri = "rtsp://example.com/live";
      request.cseq = 31;
      request.user_agent = "TurboMedia RTSP Client/1.0";
      request.content_type = "application/sdp";
      request.body = sdp_body;
      request.body_len = sizeof(sdp_body) - 1;

      len = turbo_rtsp_format_request(buffer, sizeof(buffer), &request);
      snprintf(content_length, sizeof(content_length), "Content-Length: %zu\r\n", sizeof(sdp_body) - 1);

      check(len > 0);
      check(strstr(buffer, "ANNOUNCE rtsp://example.com/live RTSP/1.0\r\n") == buffer);
      check(strstr(buffer, "CSeq: 31\r\n") != NULL);
      check(strstr(buffer, "User-Agent: TurboMedia RTSP Client/1.0\r\n") != NULL);
      check(strstr(buffer, "Content-Type: application/sdp\r\n") != NULL);
      check(strstr(buffer, content_length) != NULL);
      body = strstr(buffer, "\r\n\r\n");
      check(body != NULL);
      body += 4;
      check_str_eq(body, sdp_body);

      check_int_eq(turbo_rtsp_parse_message(buffer, (size_t)len, &consumed, &parsed), TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.request.method, TURBO_RTSP_METHOD_ANNOUNCE);
      check_int_eq((int)parsed.request.cseq, 31);
      check_size_eq(parsed.body_len, sizeof(sdp_body) - 1);
      check_size_eq(consumed, (size_t)len);
    }

    it("formats SETUP for RTP interleaved push transport") {
      turbo_rtsp_request_message_t request;
      turbo_rtsp_request_t parsed;
      char buffer[512];
      size_t consumed = 0;
      int len = 0;

      memset(&request, 0, sizeof(request));
      request.method = TURBO_RTSP_METHOD_SETUP;
      request.uri = "rtsp://example.com/live/trackID=0";
      request.cseq = 32;
      request.user_agent = "TurboMedia RTSP Client/1.0";
      request.transport = "RTP/AVP/TCP;unicast;interleaved=0-1;mode=RECORD";

      len = turbo_rtsp_format_request(buffer, sizeof(buffer), &request);

      check(len > 0);
      check(strstr(buffer, "SETUP rtsp://example.com/live/trackID=0 RTSP/1.0\r\n") == buffer);
      check(strstr(buffer, "CSeq: 32\r\n") != NULL);
      check(strstr(buffer, "User-Agent: TurboMedia RTSP Client/1.0\r\n") != NULL);
      check(strstr(buffer, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=RECORD\r\n") != NULL);
      check(strstr(buffer, "Content-Length: 0\r\n") != NULL);
      check(strstr(buffer, "\r\n\r\n") != NULL);

      check_int_eq(turbo_rtsp_parse_request(buffer, (size_t)len, &consumed, &parsed), TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.method, TURBO_RTSP_METHOD_SETUP);
      check_int_eq((int)parsed.cseq, 32);
      check_int_eq(parsed.transport_kind, TURBO_RTSP_TRANSPORT_RTP_AVP_TCP);
      check_int_eq(parsed.interleaved_rtp_channel, 0);
      check_int_eq(parsed.interleaved_rtcp_channel, 1);
      check_size_eq(consumed, (size_t)len);
    }

    it("formats RECORD with Session, Range and no request body") {
      turbo_rtsp_request_message_t request;
      turbo_rtsp_request_t parsed;
      char buffer[512];
      size_t consumed = 0;
      int len = 0;

      memset(&request, 0, sizeof(request));
      request.method = TURBO_RTSP_METHOD_RECORD;
      request.uri = "rtsp://example.com/live";
      request.cseq = 33;
      request.user_agent = "TurboMedia RTSP Client/1.0";
      request.session_id = "12345678";
      request.range = "npt=0.000-";

      len = turbo_rtsp_format_request(buffer, sizeof(buffer), &request);

      check(len > 0);
      check(strstr(buffer, "RECORD rtsp://example.com/live RTSP/1.0\r\n") == buffer);
      check(strstr(buffer, "CSeq: 33\r\n") != NULL);
      check(strstr(buffer, "User-Agent: TurboMedia RTSP Client/1.0\r\n") != NULL);
      check(strstr(buffer, "Session: 12345678\r\n") != NULL);
      check(strstr(buffer, "Range: npt=0.000-\r\n") != NULL);
      check(strstr(buffer, "Content-Length: 0\r\n") != NULL);

      check_int_eq(turbo_rtsp_parse_request(buffer, (size_t)len, &consumed, &parsed), TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.method, TURBO_RTSP_METHOD_RECORD);
      check_int_eq((int)parsed.cseq, 33);
      check_str_eq(parsed.session_id, "12345678");
      check_str_eq(parsed.range, "npt=0.000-");
      check_size_eq(consumed, (size_t)len);
    }

    it("formats TEARDOWN with Session and exact zero Content-Length") {
      turbo_rtsp_request_message_t request;
      turbo_rtsp_request_t parsed;
      char buffer[512];
      size_t consumed = 0;
      int len = 0;

      memset(&request, 0, sizeof(request));
      request.method = TURBO_RTSP_METHOD_TEARDOWN;
      request.uri = "rtsp://example.com/live";
      request.cseq = 34;
      request.user_agent = "TurboMedia RTSP Client/1.0";
      request.session_id = "12345678";

      len = turbo_rtsp_format_request(buffer, sizeof(buffer), &request);

      check(len > 0);
      check(strstr(buffer, "TEARDOWN rtsp://example.com/live RTSP/1.0\r\n") == buffer);
      check(strstr(buffer, "CSeq: 34\r\n") != NULL);
      check(strstr(buffer, "User-Agent: TurboMedia RTSP Client/1.0\r\n") != NULL);
      check(strstr(buffer, "Session: 12345678\r\n") != NULL);
      check(strstr(buffer, "Content-Length: 0\r\n") != NULL);

      check_int_eq(turbo_rtsp_parse_request(buffer, (size_t)len, &consumed, &parsed), TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.method, TURBO_RTSP_METHOD_TEARDOWN);
      check_int_eq((int)parsed.cseq, 34);
      check_str_eq(parsed.session_id, "12345678");
      check_size_eq(consumed, (size_t)len);
    }

    it("formats GET_PARAMETER keepalive with Session and no request body") {
      turbo_rtsp_request_message_t request;
      turbo_rtsp_request_t parsed;
      char buffer[512];
      size_t consumed = 0;
      int len = 0;

      memset(&request, 0, sizeof(request));
      request.method = TURBO_RTSP_METHOD_GET_PARAMETER;
      request.uri = "rtsp://example.com/live";
      request.cseq = 35;
      request.user_agent = "TurboMedia RTSP Client/1.0";
      request.session_id = "12345678";

      len = turbo_rtsp_format_request(buffer, sizeof(buffer), &request);

      check(len > 0);
      check(strstr(buffer, "GET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n") == buffer);
      check(strstr(buffer, "Session: 12345678\r\n") != NULL);
      check(strstr(buffer, "Content-Length: 0\r\n") != NULL);

      check_int_eq(turbo_rtsp_parse_request(buffer, (size_t)len, &consumed, &parsed), TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.method, TURBO_RTSP_METHOD_GET_PARAMETER);
      check_int_eq((int)parsed.cseq, 35);
      check_str_eq(parsed.session_id, "12345678");
      check_size_eq(consumed, (size_t)len);
    }

    it("formats SET_PARAMETER with text parameters body") {
      static const char body[] = "volume: 0.75\r\n";
      turbo_rtsp_request_message_t request;
      turbo_rtsp_message_t parsed;
      char buffer[512];
      size_t consumed = 0;
      int len = 0;

      memset(&request, 0, sizeof(request));
      request.method = TURBO_RTSP_METHOD_SET_PARAMETER;
      request.uri = "rtsp://example.com/live";
      request.cseq = 36;
      request.user_agent = "TurboMedia RTSP Client/1.0";
      request.session_id = "12345678";
      request.content_type = "text/parameters";
      request.body = body;
      request.body_len = sizeof(body) - 1;

      len = turbo_rtsp_format_request(buffer, sizeof(buffer), &request);

      check(len > 0);
      check(strstr(buffer, "SET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n") == buffer);
      check(strstr(buffer, "Session: 12345678\r\n") != NULL);
      check(strstr(buffer, "Content-Type: text/parameters\r\n") != NULL);
      check(strstr(buffer, "Content-Length: 14\r\n") != NULL);

      check_int_eq(turbo_rtsp_parse_message(buffer, (size_t)len, &consumed, &parsed), TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.request.method, TURBO_RTSP_METHOD_SET_PARAMETER);
      check_int_eq((int)parsed.request.cseq, 36);
      check_str_eq(parsed.request.session_id, "12345678");
      check_size_eq(parsed.body_len, sizeof(body) - 1);
      check_mem_eq(parsed.body, body, sizeof(body) - 1);
      check_size_eq(consumed, (size_t)len);
    }
  }

  section("RFC 3550 RTP/AVP transport parameters") {
    it("parses RTP/AVP over UDP client RTP and RTCP ports") {
      const char *request =
          "SETUP rtsp://example.com/live/trackID=0 RTSP/1.0\r\n"
          "cseq: 8\r\n"
          "Transport: RTP/AVP;unicast;client_port=8000-8001\r\n"
          "\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.method, TURBO_RTSP_METHOD_SETUP);
      check_int_eq((int)parsed.cseq, 8);
      check_int_eq(parsed.transport_kind, TURBO_RTSP_TRANSPORT_RTP_AVP_UDP);
      check_int_eq(parsed.transport_spec.kind, TURBO_RTSP_TRANSPORT_RTP_AVP_UDP);
      check_int_eq(parsed.transport_spec.delivery, TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST);
      check_int_eq(parsed.client_rtp_port, 8000);
      check_int_eq(parsed.client_rtcp_port, 8001);
      check_int_eq(parsed.transport_spec.client_rtp_port, 8000);
      check_int_eq(parsed.transport_spec.client_rtcp_port, 8001);
      check_int_eq(parsed.interleaved_rtp_channel, -1);
      check_int_eq(parsed.interleaved_rtcp_channel, -1);
      check_str_eq(parsed.transport, "RTP/AVP;unicast;client_port=8000-8001");
    }

    it("parses a standalone RTP/AVP transport header value") {
      turbo_rtsp_transport_spec_t parsed;

      memset(&parsed, 0, sizeof(parsed));

      check_int_eq(
          turbo_rtsp_parse_transport(
              "RTP/AVP;unicast;client_port=4588-4589;server_port=6256-6257;mode=PLAY",
              0,
              &parsed),
          0);
      check_int_eq(parsed.kind, TURBO_RTSP_TRANSPORT_RTP_AVP_UDP);
      check_int_eq(parsed.delivery, TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST);
      check_int_eq(parsed.client_rtp_port, 4588);
      check_int_eq(parsed.client_rtcp_port, 4589);
      check_int_eq(parsed.server_rtp_port, 6256);
      check_int_eq(parsed.server_rtcp_port, 6257);
      check_int_eq(parsed.mode, TURBO_RTSP_TRANSPORT_MODE_PLAY);
    }

    it("parses explicit RTP/AVP/UDP transport profiles") {
      const char *request =
          "SETUP rtsp://example.com/live/trackID=0 RTSP/1.0\r\n"
          "CSeq: 13\r\n"
          "Transport: RTP/AVP/UDP;unicast;client_port=5000-5001;server_port=6000-6001\r\n"
          "\r\n";
      turbo_rtsp_request_t request_parsed;
      turbo_rtsp_transport_spec_t parsed;
      size_t consumed = 0;

      memset(&parsed, 0, sizeof(parsed));

      check_int_eq(turbo_rtsp_parse_request(request, strlen(request), &consumed, &request_parsed), 0);
      check_int_eq(request_parsed.transport_kind, TURBO_RTSP_TRANSPORT_RTP_AVP_UDP);
      check_int_eq(request_parsed.transport_spec.client_rtp_port, 5000);
      check_int_eq(request_parsed.transport_spec.server_rtp_port, 6000);

      check_int_eq(
          turbo_rtsp_parse_transport(
              "RTP/AVP/UDP;unicast;client_port=5000-5001;server_port=6000-6001",
              0,
              &parsed),
          0);
      check_int_eq(parsed.kind, TURBO_RTSP_TRANSPORT_RTP_AVP_UDP);
      check_int_eq(parsed.delivery, TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST);
      check_int_eq(parsed.client_rtp_port, 5000);
      check_int_eq(parsed.client_rtcp_port, 5001);
      check_int_eq(parsed.server_rtp_port, 6000);
      check_int_eq(parsed.server_rtcp_port, 6001);
    }

    it("rejects invalid RTP transport profiles instead of prefix-matching them") {
      turbo_rtsp_transport_spec_t parsed;

      memset(&parsed, 0, sizeof(parsed));

      check_int_eq(
          turbo_rtsp_parse_transport(
              "RTP/AVPX;unicast;client_port=4588-4589",
              0,
              &parsed),
          -1);
    }

    it("parses interleaved RTP/AVP/TCP channel pairs") {
      const char *request =
          "SETUP rtsp://example.com/live/trackID=1 RTSP/1.0\r\n"
          "CSeq: 9\r\n"
          "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
          "\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.transport_kind, TURBO_RTSP_TRANSPORT_RTP_AVP_TCP);
      check_int_eq(parsed.transport_spec.kind, TURBO_RTSP_TRANSPORT_RTP_AVP_TCP);
      check_int_eq(parsed.client_rtp_port, -1);
      check_int_eq(parsed.client_rtcp_port, -1);
      check_int_eq(parsed.interleaved_rtp_channel, 2);
      check_int_eq(parsed.interleaved_rtcp_channel, 3);
      check_int_eq(parsed.transport_spec.interleaved_rtp_channel, 2);
      check_int_eq(parsed.transport_spec.interleaved_rtcp_channel, 3);
    }

    it("parses unicast server ports, source and SSRC transport parameters") {
      const char *request =
          "SETUP rtsp://example.com/live/trackID=0 RTSP/1.0\r\n"
          "CSeq: 10\r\n"
          "Transport: RTP/AVP;unicast;client_port=4588;server_port=6256-6257;source=192.0.2.10;ssrc=08ABE80F\r\n"
          "\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.transport_spec.delivery, TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST);
      check_int_eq(parsed.transport_spec.client_rtp_port, 4588);
      check_int_eq(parsed.transport_spec.client_rtcp_port, 4589);
      check_int_eq(parsed.transport_spec.server_rtp_port, 6256);
      check_int_eq(parsed.transport_spec.server_rtcp_port, 6257);
      check_str_eq(parsed.transport_spec.source, "192.0.2.10");
      check_int_eq(parsed.transport_spec.has_ssrc, 1);
      check_int_eq((int)parsed.transport_spec.ssrc, 0x08abe80f);
    }

    it("parses multicast destination, ports, ttl, layers and mode") {
      const char *request =
          "SETUP rtsp://example.com/live/trackID=1 RTSP/1.0\r\n"
          "CSeq: 11\r\n"
          "Transport: RTP/AVP;multicast;destination=239.255.0.1;port=3457;ttl=16;layers=2;mode=\"PLAY\"\r\n"
          "\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.transport_spec.kind, TURBO_RTSP_TRANSPORT_RTP_AVP_UDP);
      check_int_eq(parsed.transport_spec.delivery, TURBO_RTSP_TRANSPORT_DELIVERY_MULTICAST);
      check_str_eq(parsed.transport_spec.destination, "239.255.0.1");
      check_int_eq(parsed.transport_spec.multicast_rtp_port, 3456);
      check_int_eq(parsed.transport_spec.multicast_rtcp_port, 3457);
      check_int_eq(parsed.transport_spec.ttl, 16);
      check_int_eq(parsed.transport_spec.layers, 2);
      check_int_eq(parsed.transport_spec.mode, TURBO_RTSP_TRANSPORT_MODE_PLAY);
    }

    it("parses single interleaved channel as an RTP and RTCP pair") {
      const char *request =
          "SETUP rtsp://example.com/live/trackID=2 RTSP/1.0\r\n"
          "CSeq: 12\r\n"
          "Transport: RTP/AVP/TCP;unicast;interleaved=4;mode=RECORD;append\r\n"
          "\r\n";
      turbo_rtsp_request_t parsed;
      size_t consumed = 0;
      int rc = turbo_rtsp_parse_request(request, strlen(request), &consumed, &parsed);

      check_int_eq(rc, TURBO_RTSP_PARSE_OK);
      check_int_eq(parsed.transport_spec.interleaved_rtp_channel, 4);
      check_int_eq(parsed.transport_spec.interleaved_rtcp_channel, 5);
      check_int_eq(parsed.transport_spec.mode, TURBO_RTSP_TRANSPORT_MODE_RECORD);
      check_int_eq(parsed.transport_spec.append, 1);
    }

    it("encodes and decodes the fixed RTP header fields") {
      turbo_rtsp_rtp_header_t header;
      turbo_rtsp_rtp_header_t parsed;
      uint8_t packet[TURBO_RTSP_RTP_HEADER_SIZE];
      size_t header_len = 0;
      int written = 0;

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.marker = 1;
      header.payload_type = 96;
      header.sequence_number = 0x1234;
      header.timestamp = 0x01020304;
      header.ssrc = 0xaabbccdd;

      written = turbo_rtsp_rtp_write_header(packet, sizeof(packet), &header);
      check_int_eq(written, TURBO_RTSP_RTP_HEADER_SIZE);
      check_int_eq(packet[0], 0x80);
      check_int_eq(packet[1], 0xe0);
      check_int_eq(packet[2], 0x12);
      check_int_eq(packet[3], 0x34);

      check_int_eq(turbo_rtsp_rtp_parse_header(packet, sizeof(packet), &parsed, &header_len), 0);
      check_size_eq(header_len, TURBO_RTSP_RTP_HEADER_SIZE);
      check_int_eq(parsed.version, 2);
      check_int_eq(parsed.marker, 1);
      check_int_eq(parsed.payload_type, 96);
      check_int_eq(parsed.sequence_number, 0x1234);
      check_int_eq((int)parsed.timestamp, 0x01020304);
      check_int_eq((int)parsed.ssrc, (int)0xaabbccdd);
    }

    it("rejects RTP packets with unsupported version") {
      const uint8_t packet[TURBO_RTSP_RTP_HEADER_SIZE] = {
          0x40, 0x60, 0x00, 0x01, 0, 0, 0, 1, 0, 0, 0, 2
      };
      turbo_rtsp_rtp_header_t parsed;
      size_t header_len = 0;

      check_int_eq(turbo_rtsp_rtp_parse_header(packet, sizeof(packet), &parsed, &header_len), -1);
    }

    it("parses CSRC, header extension, payload and padding") {
      const uint8_t packet[] = {
          0xb1, 0xe0, 0x12, 0x34, 0x01, 0x02, 0x03, 0x04,
          0xaa, 0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33, 0x44,
          0xbe, 0xde, 0x00, 0x01, 0x55, 0x66, 0x77, 0x88,
          0xde, 0xad, 0xbe, 0x00, 0x00, 0x04
      };
      turbo_rtsp_rtp_header_t parsed;
      size_t header_len = 0;

      check_int_eq(turbo_rtsp_rtp_parse_header(packet, sizeof(packet), &parsed, &header_len), 0);
      check_size_eq(header_len, 24);
      check_int_eq(parsed.version, 2);
      check_int_eq(parsed.padding, 1);
      check_int_eq(parsed.extension, 1);
      check_int_eq(parsed.padding_len, 4);
      check_int_eq(parsed.csrc_count, 1);
      check_int_eq((int)parsed.csrc[0], 0x11223344);
      check_int_eq(parsed.extension_profile, 0xbede);
      check_size_eq(parsed.extension_len, 4);
      check_mem_eq(parsed.extension_data, packet + 20, 4);
      check_size_eq(parsed.payload_len, 2);
      check_mem_eq(parsed.payload, packet + 24, 2);
    }

    it("writes RTP headers with CSRC and header extension") {
      const uint8_t extension[] = {0x10, 0x20, 0x30, 0x40};
      const uint8_t expected[] = {
          0x91, 0x60, 0x00, 0x2a, 0x01, 0x02, 0x03, 0x04,
          0x0a, 0x0b, 0x0c, 0x0d, 0x11, 0x22, 0x33, 0x44,
          0xab, 0xcd, 0x00, 0x01, 0x10, 0x20, 0x30, 0x40
      };
      turbo_rtsp_rtp_header_t header;
      uint8_t packet[sizeof(expected)];
      int written = 0;

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.extension = 1;
      header.csrc_count = 1;
      header.payload_type = 96;
      header.sequence_number = 42;
      header.timestamp = 0x01020304;
      header.ssrc = 0x0a0b0c0d;
      header.csrc[0] = 0x11223344;
      header.extension_profile = 0xabcd;
      header.extension_data = extension;
      header.extension_len = sizeof(extension);

      written = turbo_rtsp_rtp_write_header(packet, sizeof(packet), &header);
      check_int_eq(written, (int)sizeof(expected));
      check_mem_eq(packet, expected, sizeof(expected));
    }

    it("writes complete RTP packets with payload") {
      const uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};
      turbo_rtsp_rtp_header_t header;
      turbo_rtsp_rtp_header_t parsed;
      uint8_t packet[TURBO_RTSP_RTP_HEADER_SIZE + sizeof(payload)];
      size_t header_len = 0;
      int written = 0;

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.marker = 1;
      header.payload_type = 96;
      header.sequence_number = 0x1234;
      header.timestamp = 0x01020304;
      header.ssrc = 0xaabbccdd;

      written = turbo_rtsp_rtp_write_packet(
          packet,
          sizeof(packet),
          &header,
          payload,
          sizeof(payload));
      check_int_eq(written, (int)sizeof(packet));
      check_int_eq(turbo_rtsp_rtp_parse_header(packet, (size_t)written, &parsed, &header_len), 0);
      check_size_eq(header_len, TURBO_RTSP_RTP_HEADER_SIZE);
      check_int_eq(parsed.marker, 1);
      check_int_eq(parsed.payload_type, 96);
      check_int_eq(parsed.sequence_number, 0x1234);
      check_size_eq(parsed.payload_len, sizeof(payload));
      check_mem_eq(parsed.payload, payload, sizeof(payload));
    }

    it("writes RTP packets with CSRC, extension and payload") {
      const uint8_t extension[] = {0x10, 0x20, 0x30, 0x40};
      const uint8_t payload[] = {0x01, 0x02, 0x03};
      turbo_rtsp_rtp_header_t header;
      turbo_rtsp_rtp_header_t parsed;
      uint8_t packet[24 + sizeof(payload)];
      size_t header_len = 0;
      int written = 0;

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.extension = 1;
      header.csrc_count = 1;
      header.payload_type = 97;
      header.sequence_number = 7;
      header.timestamp = 0x01020304;
      header.ssrc = 0x0a0b0c0d;
      header.csrc[0] = 0x11223344;
      header.extension_profile = 0xabcd;
      header.extension_data = extension;
      header.extension_len = sizeof(extension);

      written = turbo_rtsp_rtp_write_packet(
          packet,
          sizeof(packet),
          &header,
          payload,
          sizeof(payload));
      check_int_eq(written, (int)sizeof(packet));
      check_int_eq(turbo_rtsp_rtp_parse_header(packet, (size_t)written, &parsed, &header_len), 0);
      check_size_eq(header_len, 24);
      check_int_eq(parsed.csrc_count, 1);
      check_int_eq((int)parsed.csrc[0], 0x11223344);
      check_int_eq(parsed.extension_profile, 0xabcd);
      check_size_eq(parsed.extension_len, sizeof(extension));
      check_mem_eq(parsed.extension_data, extension, sizeof(extension));
      check_size_eq(parsed.payload_len, sizeof(payload));
      check_mem_eq(parsed.payload, payload, sizeof(payload));
    }

    it("rejects invalid RTP packet writes") {
      const uint8_t payload[] = {0xde, 0xad};
      turbo_rtsp_rtp_header_t header;
      uint8_t packet[TURBO_RTSP_RTP_HEADER_SIZE + 1];

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.payload_type = 96;

      check_int_eq(
          turbo_rtsp_rtp_write_packet(
              packet,
              sizeof(packet),
              &header,
              payload,
              sizeof(payload)),
          -1);
      header.padding = 1;
      check_int_eq(
          turbo_rtsp_rtp_write_packet(
              packet,
              sizeof(packet),
              &header,
              NULL,
              0),
          -1);
    }

    it("parses H264 single NAL RTP payloads") {
      const uint8_t payload[] = {0x65, 0x88, 0x99};
      turbo_rtsp_h264_payload_t h264;
      uint8_t nal[sizeof(payload)];
      size_t written = 0;

      memset(&h264, 0, sizeof(h264));
      memset(nal, 0, sizeof(nal));

      check_int_eq(turbo_rtsp_h264_payload_parse(payload, sizeof(payload), &h264), 0);
      check_int_eq(h264.kind, TURBO_RTSP_H264_PAYLOAD_SINGLE_NAL);
      check_int_eq(h264.forbidden_zero_bit, 0);
      check_int_eq(h264.nal_ref_idc, 3);
      check_int_eq(h264.nal_unit_type, 5);
      check_size_eq(h264.nal_len, sizeof(payload));
      check_mem_eq(h264.nal, payload, sizeof(payload));

      check_int_eq(
          turbo_rtsp_h264_payload_write_nal_fragment(
              nal,
              sizeof(nal),
              &h264,
              &written),
          0);
      check_size_eq(written, sizeof(payload));
      check_mem_eq(nal, payload, sizeof(payload));
    }

    it("iterates H264 STAP-A aggregated NAL units") {
      const uint8_t payload[] = {
          0x78,
          0x00, 0x03, 0x67, 0x42, 0x00,
          0x00, 0x02, 0x68, 0xce
      };
      turbo_rtsp_h264_payload_t h264;
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t offset = 0;

      memset(&h264, 0, sizeof(h264));

      check_int_eq(turbo_rtsp_h264_payload_parse(payload, sizeof(payload), &h264), 0);
      check_int_eq(h264.kind, TURBO_RTSP_H264_PAYLOAD_STAP_A);
      check_int_eq(h264.nal_ref_idc, 3);
      check_int_eq(h264.nal_unit_type, TURBO_RTSP_H264_NAL_TYPE_STAP_A);

      check_int_eq(turbo_rtsp_h264_stap_a_next(&h264, &offset, &nal, &nal_len), 0);
      check_size_eq(nal_len, 3);
      check_mem_eq(nal, payload + 3, 3);

      check_int_eq(turbo_rtsp_h264_stap_a_next(&h264, &offset, &nal, &nal_len), 0);
      check_size_eq(nal_len, 2);
      check_mem_eq(nal, payload + 8, 2);

      check_int_eq(turbo_rtsp_h264_stap_a_next(&h264, &offset, &nal, &nal_len), 1);
      check_size_eq(nal_len, 0);
      check(nal == NULL);
    }

    it("reassembles H264 FU-A fragments into NAL bytes") {
      const uint8_t expected_nal[] = {0x65, 0x88, 0x84, 0x21, 0xa0};
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88, 0x84};
      const uint8_t middle_payload[] = {0x7c, 0x05, 0x21};
      const uint8_t end_payload[] = {0x7c, 0x45, 0xa0};
      turbo_rtsp_h264_payload_t h264;
      uint8_t nal[sizeof(expected_nal)];
      size_t total = 0;
      size_t written = 0;

      memset(&h264, 0, sizeof(h264));
      memset(nal, 0, sizeof(nal));

      check_int_eq(turbo_rtsp_h264_payload_parse(start_payload, sizeof(start_payload), &h264), 0);
      check_int_eq(h264.kind, TURBO_RTSP_H264_PAYLOAD_FU_A);
      check_int_eq(h264.fu_start, 1);
      check_int_eq(h264.fu_end, 0);
      check_int_eq(h264.fu_nal_unit_type, 5);
      check_int_eq(h264.reconstructed_nal_header, 0x65);
      check_int_eq(
          turbo_rtsp_h264_payload_write_nal_fragment(
              nal + total,
              sizeof(nal) - total,
              &h264,
              &written),
          0);
      total += written;

      check_int_eq(turbo_rtsp_h264_payload_parse(middle_payload, sizeof(middle_payload), &h264), 0);
      check_int_eq(h264.fu_start, 0);
      check_int_eq(h264.fu_end, 0);
      check_int_eq(
          turbo_rtsp_h264_payload_write_nal_fragment(
              nal + total,
              sizeof(nal) - total,
              &h264,
              &written),
          0);
      total += written;

      check_int_eq(turbo_rtsp_h264_payload_parse(end_payload, sizeof(end_payload), &h264), 0);
      check_int_eq(h264.fu_start, 0);
      check_int_eq(h264.fu_end, 1);
      check_int_eq(
          turbo_rtsp_h264_payload_write_nal_fragment(
              nal + total,
              sizeof(nal) - total,
              &h264,
              &written),
          0);
      total += written;

      check_size_eq(total, sizeof(expected_nal));
      check_mem_eq(nal, expected_nal, sizeof(expected_nal));
    }

    it("packetizes H264 NALs as single NAL payloads when they fit") {
      const uint8_t nal[] = {0x65, 0x88, 0x99};
      turbo_rtsp_h264_packetizer_t packetizer;
      turbo_rtsp_h264_packetized_payload_t out;

      memset(&packetizer, 0, sizeof(packetizer));
      memset(&out, 0, sizeof(out));

      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              nal,
              sizeof(nal),
              sizeof(nal)),
          0);
      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              NULL,
              0,
              &out),
          0);
      check_size_eq(out.payload_len, sizeof(nal));
      check_mem_eq(out.payload, nal, sizeof(nal));
      check_int_eq(out.marker, 1);
      check_int_eq(out.end, 1);
      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              NULL,
              0,
              &out),
          1);
    }

    it("packetizes oversized H264 NALs as FU-A payloads on max payload boundaries") {
      const uint8_t nal[] = {0x65, 0x01, 0x02, 0x03, 0x04, 0x05};
      const uint8_t expected_start[] = {0x7c, 0x85, 0x01, 0x02};
      const uint8_t expected_middle[] = {0x7c, 0x05, 0x03, 0x04};
      const uint8_t expected_end[] = {0x7c, 0x45, 0x05};
      turbo_rtsp_h264_packetizer_t packetizer;
      turbo_rtsp_h264_packetized_payload_t out;
      uint8_t payload[4];

      memset(&packetizer, 0, sizeof(packetizer));
      memset(&out, 0, sizeof(out));
      memset(payload, 0, sizeof(payload));

      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              nal,
              sizeof(nal),
              sizeof(payload)),
          0);

      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          0);
      check_size_eq(out.payload_len, sizeof(expected_start));
      check_mem_eq(out.payload, expected_start, sizeof(expected_start));
      check_int_eq(out.marker, 0);
      check_int_eq(out.end, 0);

      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          0);
      check_size_eq(out.payload_len, sizeof(expected_middle));
      check_mem_eq(out.payload, expected_middle, sizeof(expected_middle));
      check_int_eq(out.marker, 0);
      check_int_eq(out.end, 0);

      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          0);
      check_size_eq(out.payload_len, sizeof(expected_end));
      check_mem_eq(out.payload, expected_end, sizeof(expected_end));
      check_int_eq(out.marker, 1);
      check_int_eq(out.end, 1);

      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          1);
    }

    it("rejects H264 packetizer MTU values too small for FU-A") {
      const uint8_t nal[] = {0x65, 0x01, 0x02};
      turbo_rtsp_h264_packetizer_t packetizer;

      memset(&packetizer, 0, sizeof(packetizer));

      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              nal,
              sizeof(nal),
              2),
          -1);
    }

    it("rejects empty H264 NALs for packetization") {
      const uint8_t nal[] = {0x65};
      turbo_rtsp_h264_packetizer_t packetizer;

      memset(&packetizer, 0, sizeof(packetizer));

      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              nal,
              0,
              1200),
          -1);
    }

    it("statefully reassembles H264 FU-A start middle and end RTP packets") {
      const uint8_t expected_nal[] = {0x65, 0x88, 0x84, 0x21, 0xa0};
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88, 0x84};
      const uint8_t middle_payload[] = {0x7c, 0x05, 0x21};
      const uint8_t end_payload[] = {0x7c, 0x45, 0xa0};
      turbo_rtsp_h264_reassembler_t reassembler;
      turbo_rtsp_rtp_header_t header;
      uint8_t nal_buffer[sizeof(expected_nal)];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;

      turbo_rtsp_h264_reassembler_init(&reassembler);
      memset(nal_buffer, 0, sizeof(nal_buffer));

      turbo_rtsp_test_rtp_payload_header(
          &header,
          100,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check_int_eq(
          turbo_rtsp_h264_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_PARTIAL);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
      check_int_eq(reassembler.started, 1);

      turbo_rtsp_test_rtp_payload_header(
          &header,
          101,
          90000,
          0x01020304,
          middle_payload,
          sizeof(middle_payload));
      check_int_eq(
          turbo_rtsp_h264_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_PARTIAL);
      check(nal == NULL);
      check_size_eq(nal_len, 0);

      turbo_rtsp_test_rtp_payload_header(
          &header,
          102,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));
      check_int_eq(
          turbo_rtsp_h264_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_OK);
      check(nal == nal_buffer);
      check_size_eq(nal_len, sizeof(expected_nal));
      check_mem_eq(nal, expected_nal, sizeof(expected_nal));
      check_int_eq(reassembler.started, 0);
    }

    it("rejects H264 FU-A end packets without a started reassembly") {
      const uint8_t end_payload[] = {0x7c, 0x45, 0xa0};
      turbo_rtsp_h264_reassembler_t reassembler;
      turbo_rtsp_rtp_header_t header;
      uint8_t nal_buffer[8];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;

      turbo_rtsp_h264_reassembler_init(&reassembler);
      turbo_rtsp_test_rtp_payload_header(
          &header,
          10,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));

      check_int_eq(
          turbo_rtsp_h264_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_ERROR);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
      check_int_eq(reassembler.started, 0);
    }

    it("rejects H264 FU-A RTP sequence gaps") {
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88};
      const uint8_t end_payload[] = {0x7c, 0x45, 0xa0};
      turbo_rtsp_h264_reassembler_t reassembler;
      turbo_rtsp_rtp_header_t header;
      uint8_t nal_buffer[8];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;

      turbo_rtsp_h264_reassembler_init(&reassembler);
      turbo_rtsp_test_rtp_payload_header(
          &header,
          20,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check_int_eq(
          turbo_rtsp_h264_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_PARTIAL);

      turbo_rtsp_test_rtp_payload_header(
          &header,
          22,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));
      check_int_eq(
          turbo_rtsp_h264_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_ERROR);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
      check_int_eq(reassembler.started, 0);
    }

    it("rejects H264 FU-A nal type changes during reassembly") {
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88};
      const uint8_t changed_type_payload[] = {0x7c, 0x01, 0x21};
      turbo_rtsp_h264_reassembler_t reassembler;
      turbo_rtsp_rtp_header_t header;
      uint8_t nal_buffer[8];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;

      turbo_rtsp_h264_reassembler_init(&reassembler);
      turbo_rtsp_test_rtp_payload_header(
          &header,
          30,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check_int_eq(
          turbo_rtsp_h264_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_PARTIAL);

      turbo_rtsp_test_rtp_payload_header(
          &header,
          31,
          90000,
          0x01020304,
          changed_type_payload,
          sizeof(changed_type_payload));
      check_int_eq(
          turbo_rtsp_h264_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_ERROR);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
      check_int_eq(reassembler.started, 0);
    }

    it("statefully outputs H264 single NAL RTP payloads directly") {
      const uint8_t payload[] = {0x41, 0x9a, 0xbc};
      turbo_rtsp_h264_reassembler_t reassembler;
      turbo_rtsp_rtp_header_t header;
      const uint8_t *nal = NULL;
      size_t nal_len = 0;

      turbo_rtsp_h264_reassembler_init(&reassembler);
      turbo_rtsp_test_rtp_payload_header(
          &header,
          40,
          90000,
          0x01020304,
          payload,
          sizeof(payload));

      check_int_eq(
          turbo_rtsp_h264_reassembler_push(
              &reassembler,
              &header,
              NULL,
              0,
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_OK);
      check(nal == payload);
      check_size_eq(nal_len, sizeof(payload));
      check_mem_eq(nal, payload, sizeof(payload));
      check_int_eq(reassembler.started, 0);
    }

    it("packetizes H264 single NAL payloads without copying") {
      const uint8_t nal[] = {0x65, 0x88, 0x99};
      turbo_rtsp_h264_packetizer_t packetizer;
      turbo_rtsp_h264_packetized_payload_t out;

      memset(&packetizer, 0, sizeof(packetizer));
      memset(&out, 0, sizeof(out));

      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              nal,
              sizeof(nal),
              sizeof(nal)),
          0);
      check_int_eq(
          turbo_rtsp_h264_packetizer_next(&packetizer, NULL, 0, &out),
          0);
      check(out.payload == nal);
      check_size_eq(out.payload_len, sizeof(nal));
      check_int_eq(out.marker, 1);
      check_int_eq(out.end, 1);
      check_mem_eq(out.payload, nal, sizeof(nal));
      check_int_eq(
          turbo_rtsp_h264_packetizer_next(&packetizer, NULL, 0, &out),
          1);
    }

    it("packetizes H264 NAL units as FU-A start middle and end payloads") {
      const uint8_t nal[] = {0x65, 0x11, 0x22, 0x33, 0x44, 0x55};
      const uint8_t expected_start[] = {0x7c, 0x85, 0x11, 0x22};
      const uint8_t expected_middle[] = {0x7c, 0x05, 0x33, 0x44};
      const uint8_t expected_end[] = {0x7c, 0x45, 0x55};
      turbo_rtsp_h264_packetizer_t packetizer;
      turbo_rtsp_h264_packetized_payload_t out;
      uint8_t payload[4];

      memset(&packetizer, 0, sizeof(packetizer));
      memset(&out, 0, sizeof(out));
      memset(payload, 0, sizeof(payload));

      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              nal,
              sizeof(nal),
              4),
          0);

      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          0);
      check(out.payload == payload);
      check_size_eq(out.payload_len, sizeof(expected_start));
      check_int_eq(out.marker, 0);
      check_int_eq(out.end, 0);
      check_mem_eq(out.payload, expected_start, sizeof(expected_start));

      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          0);
      check_size_eq(out.payload_len, sizeof(expected_middle));
      check_int_eq(out.marker, 0);
      check_int_eq(out.end, 0);
      check_mem_eq(out.payload, expected_middle, sizeof(expected_middle));

      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          0);
      check_size_eq(out.payload_len, sizeof(expected_end));
      check_int_eq(out.marker, 1);
      check_int_eq(out.end, 1);
      check_mem_eq(out.payload, expected_end, sizeof(expected_end));

      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          1);
    }

    it("keeps H264 packetizer state when the output buffer is too small") {
      const uint8_t nal[] = {0x41, 0xaa, 0xbb, 0xcc, 0xdd};
      const uint8_t expected[] = {0x5c, 0x81, 0xaa, 0xbb};
      turbo_rtsp_h264_packetizer_t packetizer;
      turbo_rtsp_h264_packetized_payload_t out;
      uint8_t small_payload[3];
      uint8_t payload[4];

      memset(&packetizer, 0, sizeof(packetizer));
      memset(&out, 0, sizeof(out));
      memset(small_payload, 0, sizeof(small_payload));
      memset(payload, 0, sizeof(payload));

      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              nal,
              sizeof(nal),
              4),
          0);
      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              small_payload,
              sizeof(small_payload),
              &out),
          -1);
      check_int_eq(
          turbo_rtsp_h264_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          0);
      check_size_eq(out.payload_len, sizeof(expected));
      check_int_eq(out.marker, 0);
      check_int_eq(out.end, 0);
      check_mem_eq(out.payload, expected, sizeof(expected));
    }

    it("rejects H264 packetizer inputs that cannot form valid RTP payloads") {
      const uint8_t empty_nal[] = {0};
      const uint8_t nal[] = {0x65, 0x11, 0x22, 0x33};
      const uint8_t forbidden_nal[] = {0x85, 0x11};
      const uint8_t aggregate_nal[] = {0x78, 0x00, 0x01, 0x67};
      turbo_rtsp_h264_packetizer_t packetizer;

      memset(&packetizer, 0, sizeof(packetizer));

      check_int_eq(
          turbo_rtsp_h264_packetizer_init(&packetizer, NULL, 1, 4),
          -1);
      check_int_eq(
          turbo_rtsp_h264_packetizer_init(&packetizer, empty_nal, 0, 4),
          -1);
      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              nal,
              sizeof(nal),
              0),
          -1);
      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              nal,
              sizeof(nal),
              2),
          -1);
      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              forbidden_nal,
              sizeof(forbidden_nal),
              4),
          -1);
      check_int_eq(
          turbo_rtsp_h264_packetizer_init(
              &packetizer,
              aggregate_nal,
              sizeof(aggregate_nal),
              4),
          -1);
    }

    it("rejects malformed or unsupported H264 RTP payloads") {
      const uint8_t forbidden_bit_payload[] = {0x85};
      const uint8_t unsupported_payload[] = {0x79};
      const uint8_t short_fu_a[] = {0x7c};
      const uint8_t invalid_fu_a_flags[] = {0x7c, 0xc5};
      const uint8_t invalid_fu_a_reserved[] = {0x7c, 0xa5};
      const uint8_t invalid_fu_a_type[] = {0x7c, 0x98};
      const uint8_t malformed_stap_a[] = {0x78, 0x00, 0x04, 0x67, 0x42};
      turbo_rtsp_h264_payload_t h264;
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t offset = 0;
      size_t written = 0;
      uint8_t buffer[2];

      memset(&h264, 0, sizeof(h264));

      check_int_eq(turbo_rtsp_h264_payload_parse(NULL, 0, &h264), -1);
      check_int_eq(turbo_rtsp_h264_payload_parse(forbidden_bit_payload, sizeof(forbidden_bit_payload), &h264), -1);
      check_int_eq(turbo_rtsp_h264_payload_parse(unsupported_payload, sizeof(unsupported_payload), &h264), -1);
      check_int_eq(turbo_rtsp_h264_payload_parse(short_fu_a, sizeof(short_fu_a), &h264), -1);
      check_int_eq(turbo_rtsp_h264_payload_parse(invalid_fu_a_flags, sizeof(invalid_fu_a_flags), &h264), -1);
      check_int_eq(turbo_rtsp_h264_payload_parse(invalid_fu_a_reserved, sizeof(invalid_fu_a_reserved), &h264), -1);
      check_int_eq(turbo_rtsp_h264_payload_parse(invalid_fu_a_type, sizeof(invalid_fu_a_type), &h264), -1);

      check_int_eq(turbo_rtsp_h264_payload_parse(malformed_stap_a, sizeof(malformed_stap_a), &h264), 0);
      check_int_eq(turbo_rtsp_h264_stap_a_next(&h264, &offset, &nal, &nal_len), -1);
      check_int_eq(
          turbo_rtsp_h264_payload_write_nal_fragment(
              buffer,
              sizeof(buffer),
              &h264,
              &written),
          -1);
    }

    it("parses H265 single NAL RTP payloads") {
      const uint8_t payload[] = {0x26, 0x01, 0x88, 0x99};
      turbo_rtsp_h265_payload_t h265;
      uint8_t nal[sizeof(payload)];
      size_t written = 0;

      memset(&h265, 0, sizeof(h265));
      memset(nal, 0, sizeof(nal));

      check_int_eq(turbo_rtsp_h265_payload_parse(payload, sizeof(payload), &h265), 0);
      check_int_eq(h265.kind, TURBO_RTSP_H265_PAYLOAD_SINGLE_NAL);
      check_int_eq(h265.forbidden_zero_bit, 0);
      check_int_eq(h265.nal_unit_type, 19);
      check_int_eq(h265.nuh_layer_id, 0);
      check_int_eq(h265.nuh_temporal_id_plus1, 1);
      check_size_eq(h265.nal_len, sizeof(payload));
      check_mem_eq(h265.nal, payload, sizeof(payload));

      check_int_eq(
          turbo_rtsp_h265_payload_write_nal_fragment(
              nal,
              sizeof(nal),
              &h265,
              &written),
          0);
      check_size_eq(written, sizeof(payload));
      check_mem_eq(nal, payload, sizeof(payload));
    }

    it("iterates H265 AP aggregated NAL units") {
      const uint8_t payload[] = {
          0x60, 0x01,
          0x00, 0x03, 0x40, 0x01, 0xaa,
          0x00, 0x03, 0x42, 0x01, 0xbb
      };
      turbo_rtsp_h265_payload_t h265;
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t offset = 0;

      memset(&h265, 0, sizeof(h265));

      check_int_eq(turbo_rtsp_h265_payload_parse(payload, sizeof(payload), &h265), 0);
      check_int_eq(h265.kind, TURBO_RTSP_H265_PAYLOAD_AP);
      check_int_eq(h265.nal_unit_type, TURBO_RTSP_H265_NAL_TYPE_AP);

      check_int_eq(turbo_rtsp_h265_ap_next(&h265, &offset, &nal, &nal_len), 0);
      check_size_eq(nal_len, 3);
      check_mem_eq(nal, payload + 4, 3);

      check_int_eq(turbo_rtsp_h265_ap_next(&h265, &offset, &nal, &nal_len), 0);
      check_size_eq(nal_len, 3);
      check_mem_eq(nal, payload + 9, 3);

      check_int_eq(turbo_rtsp_h265_ap_next(&h265, &offset, &nal, &nal_len), 1);
      check_size_eq(nal_len, 0);
      check(nal == NULL);
    }

    it("reassembles H265 FU fragments into NAL bytes") {
      const uint8_t expected_nal[] = {0x26, 0x01, 0x88, 0x84, 0x21, 0xa0};
      const uint8_t start_payload[] = {0x62, 0x01, 0x93, 0x88, 0x84};
      const uint8_t middle_payload[] = {0x62, 0x01, 0x13, 0x21};
      const uint8_t end_payload[] = {0x62, 0x01, 0x53, 0xa0};
      turbo_rtsp_h265_payload_t h265;
      uint8_t nal[sizeof(expected_nal)];
      size_t total = 0;
      size_t written = 0;

      memset(&h265, 0, sizeof(h265));
      memset(nal, 0, sizeof(nal));

      check_int_eq(turbo_rtsp_h265_payload_parse(start_payload, sizeof(start_payload), &h265), 0);
      check_int_eq(h265.kind, TURBO_RTSP_H265_PAYLOAD_FU);
      check_int_eq(h265.fu_start, 1);
      check_int_eq(h265.fu_end, 0);
      check_int_eq(h265.fu_nal_unit_type, 19);
      check_mem_eq(h265.reconstructed_nal_header, expected_nal, 2);
      check_int_eq(
          turbo_rtsp_h265_payload_write_nal_fragment(
              nal + total,
              sizeof(nal) - total,
              &h265,
              &written),
          0);
      total += written;

      check_int_eq(turbo_rtsp_h265_payload_parse(middle_payload, sizeof(middle_payload), &h265), 0);
      check_int_eq(h265.fu_start, 0);
      check_int_eq(h265.fu_end, 0);
      check_int_eq(
          turbo_rtsp_h265_payload_write_nal_fragment(
              nal + total,
              sizeof(nal) - total,
              &h265,
              &written),
          0);
      total += written;

      check_int_eq(turbo_rtsp_h265_payload_parse(end_payload, sizeof(end_payload), &h265), 0);
      check_int_eq(h265.fu_start, 0);
      check_int_eq(h265.fu_end, 1);
      check_int_eq(
          turbo_rtsp_h265_payload_write_nal_fragment(
              nal + total,
              sizeof(nal) - total,
              &h265,
              &written),
          0);
      total += written;

      check_size_eq(total, sizeof(expected_nal));
      check_mem_eq(nal, expected_nal, sizeof(expected_nal));
    }

    it("packetizes H265 NAL units as FU start middle and end payloads") {
      const uint8_t nal[] = {0x26, 0x01, 0x11, 0x22, 0x33, 0x44, 0x55};
      const uint8_t expected_start[] = {0x62, 0x01, 0x93, 0x11, 0x22};
      const uint8_t expected_middle[] = {0x62, 0x01, 0x13, 0x33, 0x44};
      const uint8_t expected_end[] = {0x62, 0x01, 0x53, 0x55};
      turbo_rtsp_h265_packetizer_t packetizer;
      turbo_rtsp_h265_packetized_payload_t out;
      uint8_t payload[5];

      memset(&packetizer, 0, sizeof(packetizer));
      memset(&out, 0, sizeof(out));
      memset(payload, 0, sizeof(payload));

      check_int_eq(
          turbo_rtsp_h265_packetizer_init(
              &packetizer,
              nal,
              sizeof(nal),
              sizeof(payload)),
          0);

      check_int_eq(
          turbo_rtsp_h265_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          0);
      check_size_eq(out.payload_len, sizeof(expected_start));
      check_int_eq(out.marker, 0);
      check_int_eq(out.end, 0);
      check_mem_eq(out.payload, expected_start, sizeof(expected_start));

      check_int_eq(
          turbo_rtsp_h265_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          0);
      check_size_eq(out.payload_len, sizeof(expected_middle));
      check_int_eq(out.marker, 0);
      check_int_eq(out.end, 0);
      check_mem_eq(out.payload, expected_middle, sizeof(expected_middle));

      check_int_eq(
          turbo_rtsp_h265_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          0);
      check_size_eq(out.payload_len, sizeof(expected_end));
      check_int_eq(out.marker, 1);
      check_int_eq(out.end, 1);
      check_mem_eq(out.payload, expected_end, sizeof(expected_end));

      check_int_eq(
          turbo_rtsp_h265_packetizer_next(
              &packetizer,
              payload,
              sizeof(payload),
              &out),
          1);
    }

    it("statefully reassembles H265 FU RTP packets") {
      const uint8_t expected_nal[] = {0x26, 0x01, 0x88, 0x84, 0x21, 0xa0};
      const uint8_t start_payload[] = {0x62, 0x01, 0x93, 0x88, 0x84};
      const uint8_t middle_payload[] = {0x62, 0x01, 0x13, 0x21};
      const uint8_t end_payload[] = {0x62, 0x01, 0x53, 0xa0};
      turbo_rtsp_h265_reassembler_t reassembler;
      turbo_rtsp_rtp_header_t header;
      uint8_t nal_buffer[sizeof(expected_nal)];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;

      turbo_rtsp_h265_reassembler_init(&reassembler);
      memset(nal_buffer, 0, sizeof(nal_buffer));

      turbo_rtsp_test_rtp_payload_header(
          &header,
          100,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check_int_eq(
          turbo_rtsp_h265_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_PARTIAL);

      turbo_rtsp_test_rtp_payload_header(
          &header,
          101,
          90000,
          0x01020304,
          middle_payload,
          sizeof(middle_payload));
      check_int_eq(
          turbo_rtsp_h265_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_PARTIAL);

      turbo_rtsp_test_rtp_payload_header(
          &header,
          102,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));
      check_int_eq(
          turbo_rtsp_h265_reassembler_push(
              &reassembler,
              &header,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_OK);
      check(nal == nal_buffer);
      check_size_eq(nal_len, sizeof(expected_nal));
      check_mem_eq(nal, expected_nal, sizeof(expected_nal));
      check_int_eq(reassembler.started, 0);
    }

    it("rejects malformed or unsupported H265 RTP payloads") {
      const uint8_t short_payload[] = {0x26};
      const uint8_t forbidden_payload[] = {0xa6, 0x01};
      const uint8_t zero_tid_payload[] = {0x26, 0x00};
      const uint8_t short_fu[] = {0x62, 0x01, 0x93};
      const uint8_t invalid_fu_flags[] = {0x62, 0x01, 0xd3, 0xaa};
      const uint8_t invalid_fu_type[] = {0x62, 0x01, 0xb1, 0xaa};
      const uint8_t malformed_ap[] = {0x60, 0x01, 0x00, 0x03, 0x40};
      turbo_rtsp_h265_payload_t h265;
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t offset = 0;
      size_t written = 0;
      uint8_t buffer[2];

      memset(&h265, 0, sizeof(h265));

      check_int_eq(turbo_rtsp_h265_payload_parse(NULL, 0, &h265), -1);
      check_int_eq(turbo_rtsp_h265_payload_parse(short_payload, sizeof(short_payload), &h265), -1);
      check_int_eq(turbo_rtsp_h265_payload_parse(forbidden_payload, sizeof(forbidden_payload), &h265), -1);
      check_int_eq(turbo_rtsp_h265_payload_parse(zero_tid_payload, sizeof(zero_tid_payload), &h265), -1);
      check_int_eq(turbo_rtsp_h265_payload_parse(short_fu, sizeof(short_fu), &h265), -1);
      check_int_eq(turbo_rtsp_h265_payload_parse(invalid_fu_flags, sizeof(invalid_fu_flags), &h265), -1);
      check_int_eq(turbo_rtsp_h265_payload_parse(invalid_fu_type, sizeof(invalid_fu_type), &h265), -1);

      check_int_eq(turbo_rtsp_h265_payload_parse(malformed_ap, sizeof(malformed_ap), &h265), 0);
      check_int_eq(turbo_rtsp_h265_ap_next(&h265, &offset, &nal, &nal_len), -1);
      check_int_eq(
          turbo_rtsp_h265_payload_write_nal_fragment(
              buffer,
              sizeof(buffer),
              &h265,
              &written),
          -1);
    }

    it("parses MPEG4-GENERIC AAC-hbr RTP payloads") {
      static const uint8_t complete_payload[] = {0x00, 0x10, 0x00, 0x18, 0x11, 0x22, 0x33};
      static const uint8_t fragment_payload[] = {0x00, 0x10, 0x01, 0x48, 0xa0, 0xa1};
      static turbo_rtsp_mpeg4_generic_payload_t mpeg4;

      memset(&mpeg4, 0, sizeof(mpeg4));
      check_int_eq(
          turbo_rtsp_mpeg4_generic_payload_parse(
              complete_payload,
              sizeof(complete_payload),
              &mpeg4),
          0);
      check_int_eq(mpeg4.au_header_bits, 16);
      check_size_eq(mpeg4.au_size, 3);
      check_int_eq(mpeg4.au_index, 0);
      check_size_eq(mpeg4.au_fragment_len, 3);
      check_mem_eq(mpeg4.au_fragment, complete_payload + 4, 3);
      check_int_eq(mpeg4.complete, 1);

      check_int_eq(
          turbo_rtsp_mpeg4_generic_payload_parse(
              fragment_payload,
              sizeof(fragment_payload),
              &mpeg4),
          0);
      check_int_eq(mpeg4.au_header_bits, 16);
      check_size_eq(mpeg4.au_size, 9);
      check_int_eq(mpeg4.au_index, 0);
      check_size_eq(mpeg4.au_fragment_len, 2);
      check_mem_eq(mpeg4.au_fragment, fragment_payload + 4, 2);
      check_int_eq(mpeg4.complete, 0);
    }

    it("rejects malformed MPEG4-GENERIC AAC-hbr RTP payloads") {
      static const uint8_t short_payload[] = {0x00, 0x10, 0x00};
      static const uint8_t unsupported_header_len[] = {0x00, 0x08, 0x00, 0x18, 0x11, 0x22, 0x33};
      static const uint8_t zero_au_size[] = {0x00, 0x10, 0x00, 0x00, 0x11};
      static const uint8_t empty_fragment[] = {0x00, 0x10, 0x00, 0x18};
      static const uint8_t fragment_too_large[] = {0x00, 0x10, 0x00, 0x08, 0x11, 0x22};
      static turbo_rtsp_mpeg4_generic_payload_t mpeg4;

      memset(&mpeg4, 0, sizeof(mpeg4));
      check_int_eq(turbo_rtsp_mpeg4_generic_payload_parse(NULL, 0, &mpeg4), -1);
      check_int_eq(
          turbo_rtsp_mpeg4_generic_payload_parse(
              short_payload,
              sizeof(short_payload),
              &mpeg4),
          -1);
      check_int_eq(
          turbo_rtsp_mpeg4_generic_payload_parse(
              unsupported_header_len,
              sizeof(unsupported_header_len),
              &mpeg4),
          -1);
      check_int_eq(
          turbo_rtsp_mpeg4_generic_payload_parse(
              zero_au_size,
              sizeof(zero_au_size),
              &mpeg4),
          -1);
      check_int_eq(
          turbo_rtsp_mpeg4_generic_payload_parse(
              empty_fragment,
              sizeof(empty_fragment),
              &mpeg4),
          -1);
      check_int_eq(
          turbo_rtsp_mpeg4_generic_payload_parse(
              fragment_too_large,
              sizeof(fragment_too_large),
              &mpeg4),
          -1);
    }

    it("parses MP4A-LATM RTP payload length info") {
      static const uint8_t complete_payload[] = {0x04, 0x11, 0x22, 0x33, 0x44};
      static const uint8_t fragment_payload[] = {0xff, 0x2d, 0xa0, 0xa1, 0xa2};
      static turbo_rtsp_mp4a_latm_payload_t latm;

      memset(&latm, 0, sizeof(latm));
      check_int_eq(
          turbo_rtsp_mp4a_latm_payload_parse(
              complete_payload,
              sizeof(complete_payload),
              &latm),
          0);
      check_size_eq(latm.payload_length, 4);
      check_size_eq(latm.length_info_len, 1);
      check_mem_eq(latm.fragment, complete_payload + 1, 4);
      check_size_eq(latm.fragment_len, 4);
      check_int_eq(latm.complete, 1);

      check_int_eq(
          turbo_rtsp_mp4a_latm_payload_parse(
              fragment_payload,
              sizeof(fragment_payload),
              &latm),
          0);
      check_size_eq(latm.payload_length, 300);
      check_size_eq(latm.length_info_len, 2);
      check_mem_eq(latm.fragment, fragment_payload + 2, 3);
      check_size_eq(latm.fragment_len, 3);
      check_int_eq(latm.complete, 0);
    }

    it("rejects malformed MP4A-LATM RTP payload length info") {
      static uint8_t unterminated[TURBO_RTSP_MP4A_LATM_MAX_LENGTH_INFO_SIZE + 1];
      static const uint8_t zero_length[] = {0x00};
      static const uint8_t too_short_fragment[] = {0x04};
      static const uint8_t fragment_too_large[] = {0x01, 0x11, 0x22};
      static turbo_rtsp_mp4a_latm_payload_t latm;

      memset(&latm, 0, sizeof(latm));
      memset(unterminated, 0xff, sizeof(unterminated));

      check_int_eq(turbo_rtsp_mp4a_latm_payload_parse(NULL, 0, &latm), -1);
      check_int_eq(
          turbo_rtsp_mp4a_latm_payload_parse(
              unterminated,
              sizeof(unterminated),
              &latm),
          -1);
      check_int_eq(
          turbo_rtsp_mp4a_latm_payload_parse(
              zero_length,
              sizeof(zero_length),
              &latm),
          -1);
      check_int_eq(
          turbo_rtsp_mp4a_latm_payload_parse(
              too_short_fragment,
              sizeof(too_short_fragment),
              &latm),
          -1);
      check_int_eq(
          turbo_rtsp_mp4a_latm_payload_parse(
              fragment_too_large,
              sizeof(fragment_too_large),
              &latm),
          -1);
    }

    it("parses MPEG2 TS RTP payloads on TS packet boundaries") {
      static uint8_t ts_data[TURBO_RTSP_MPEG2_TS_PACKET_SIZE * 2];
      static turbo_rtsp_mpeg2_ts_payload_t ts;

      turbo_rtsp_test_fill_mpeg2_ts(ts_data, sizeof(ts_data));
      memset(&ts, 0, sizeof(ts));

      check_int_eq(
          turbo_rtsp_mpeg2_ts_payload_parse(ts_data, sizeof(ts_data), &ts),
          0);
      check(ts.packets == ts_data);
      check_size_eq(ts.packets_len, sizeof(ts_data));
      check_size_eq(ts.packet_count, 2);
    }

    it("rejects malformed MPEG2 TS RTP payloads") {
      static uint8_t ts_data[TURBO_RTSP_MPEG2_TS_PACKET_SIZE * 2];
      static turbo_rtsp_mpeg2_ts_payload_t ts;

      turbo_rtsp_test_fill_mpeg2_ts(ts_data, sizeof(ts_data));
      memset(&ts, 0, sizeof(ts));

      check_int_eq(turbo_rtsp_mpeg2_ts_payload_parse(NULL, 0, &ts), -1);
      check_int_eq(
          turbo_rtsp_mpeg2_ts_payload_parse(
              ts_data,
              sizeof(ts_data) - 1,
              &ts),
          -1);
      ts_data[TURBO_RTSP_MPEG2_TS_PACKET_SIZE] = 0x00;
      check_int_eq(
          turbo_rtsp_mpeg2_ts_payload_parse(ts_data, sizeof(ts_data), &ts),
          -1);
    }

    it("parses RTCP receiver report headers and report blocks") {
      const uint8_t packet[] = {
          0x81, TURBO_RTSP_RTCP_RR, 0x00, 0x07,
          0x0a, 0x0b, 0x0c, 0x0d,
          0x01, 0x02, 0x03, 0x04,
          0x05, 0xff, 0xff, 0xfe,
          0x00, 0x00, 0x12, 0x34,
          0x01, 0x02, 0x03, 0x04,
          0x11, 0x12, 0x13, 0x14,
          0x21, 0x22, 0x23, 0x24
      };
      turbo_rtsp_rtcp_header_t header;
      turbo_rtsp_rtcp_report_block_t block;

      check_int_eq(turbo_rtsp_rtcp_parse_header(packet, sizeof(packet), &header), 0);
      check_int_eq(header.version, 2);
      check_int_eq(header.count, 1);
      check_int_eq(header.packet_type, TURBO_RTSP_RTCP_RR);
      check_int_eq(header.length, 7);
      check_size_eq(header.packet_len, sizeof(packet));

      check_int_eq(
          turbo_rtsp_rtcp_parse_report_block(
              packet + TURBO_RTSP_RTCP_HEADER_SIZE + 4,
              sizeof(packet) - TURBO_RTSP_RTCP_HEADER_SIZE - 4,
              &block),
          0);
      check_int_eq((int)block.ssrc, 0x01020304);
      check_int_eq(block.fraction_lost, 5);
      check_int_eq(block.cumulative_lost, -2);
      check_int_eq((int)block.extended_highest_sequence_number, 0x00001234);
      check_int_eq((int)block.jitter, 0x01020304);
      check_int_eq((int)block.last_sender_report, 0x11121314);
      check_int_eq((int)block.delay_since_last_sender_report, 0x21222324);
    }

    it("writes RTCP sender report fields") {
      const uint8_t expected[] = {
          0x80, TURBO_RTSP_RTCP_SR, 0x00, 0x06,
          0x01, 0x02, 0x03, 0x04,
          0x11, 0x12, 0x13, 0x14,
          0x21, 0x22, 0x23, 0x24,
          0x31, 0x32, 0x33, 0x34,
          0x00, 0x00, 0x00, 0x05,
          0x00, 0x00, 0x10, 0x00
      };
      turbo_rtsp_rtcp_header_t header;
      turbo_rtsp_rtcp_sender_info_t info;
      turbo_rtsp_rtcp_sender_info_t parsed;
      uint8_t packet[sizeof(expected)];

      memset(&header, 0, sizeof(header));
      memset(&info, 0, sizeof(info));
      header.version = 2;
      header.packet_type = TURBO_RTSP_RTCP_SR;
      header.length = 6;
      info.ssrc = 0x01020304;
      info.ntp_timestamp = 0x1112131421222324ull;
      info.rtp_timestamp = 0x31323334;
      info.packet_count = 5;
      info.octet_count = 4096;

      check_int_eq(turbo_rtsp_rtcp_write_header(packet, sizeof(packet), &header), 4);
      check_int_eq(
          turbo_rtsp_rtcp_write_sender_info(
              packet + TURBO_RTSP_RTCP_HEADER_SIZE,
              sizeof(packet) - TURBO_RTSP_RTCP_HEADER_SIZE,
              &info),
          TURBO_RTSP_RTCP_SENDER_INFO_SIZE);
      check_mem_eq(packet, expected, sizeof(expected));

      check_int_eq(
          turbo_rtsp_rtcp_parse_sender_info(
              packet + TURBO_RTSP_RTCP_HEADER_SIZE,
              sizeof(packet) - TURBO_RTSP_RTCP_HEADER_SIZE,
              &parsed),
          0);
      check_int_eq((int)parsed.ssrc, 0x01020304);
      check(parsed.ntp_timestamp == 0x1112131421222324ull);
      check_int_eq((int)parsed.rtp_timestamp, 0x31323334);
      check_int_eq((int)parsed.packet_count, 5);
      check_int_eq((int)parsed.octet_count, 4096);
    }

    it("formats and parses complete RTCP receiver reports") {
      const uint8_t expected[] = {
          0x81, TURBO_RTSP_RTCP_RR, 0x00, 0x07,
          0x0a, 0x0b, 0x0c, 0x0d,
          0x01, 0x02, 0x03, 0x04,
          0x05, 0xff, 0xff, 0xfe,
          0x00, 0x00, 0x12, 0x34,
          0x01, 0x02, 0x03, 0x04,
          0x11, 0x12, 0x13, 0x14,
          0x21, 0x22, 0x23, 0x24
      };
      turbo_rtsp_rtcp_report_block_t block;
      turbo_rtsp_rtcp_report_block_t parsed_block;
      uint8_t packet[sizeof(expected)];
      uint32_t reporter_ssrc = 0;
      size_t block_count = 0;

      memset(&block, 0, sizeof(block));
      memset(&parsed_block, 0, sizeof(parsed_block));
      block.ssrc = 0x01020304;
      block.fraction_lost = 5;
      block.cumulative_lost = -2;
      block.extended_highest_sequence_number = 0x00001234;
      block.jitter = 0x01020304;
      block.last_sender_report = 0x11121314;
      block.delay_since_last_sender_report = 0x21222324;

      check_int_eq(
          turbo_rtsp_rtcp_write_receiver_report(
              packet,
              sizeof(packet),
              0x0a0b0c0d,
              &block,
              1),
          (int)sizeof(expected));
      check_mem_eq(packet, expected, sizeof(expected));

      check_int_eq(
          turbo_rtsp_rtcp_parse_receiver_report(
              packet,
              sizeof(packet),
              &reporter_ssrc,
              &parsed_block,
              1,
              &block_count),
          0);
      check_int_eq((int)reporter_ssrc, 0x0a0b0c0d);
      check_size_eq(block_count, 1);
      check_int_eq((int)parsed_block.ssrc, 0x01020304);
      check_int_eq(parsed_block.fraction_lost, 5);
      check_int_eq(parsed_block.cumulative_lost, -2);
      check_int_eq((int)parsed_block.extended_highest_sequence_number, 0x00001234);

      block_count = 0;
      check_int_eq(
          turbo_rtsp_rtcp_parse_receiver_report(
              packet,
              sizeof(packet),
              &reporter_ssrc,
              NULL,
              0,
              &block_count),
          -1);
      check_size_eq(block_count, 1);
    }

    it("formats and parses complete RTCP sender reports with report blocks") {
      turbo_rtsp_rtcp_sender_info_t sender;
      turbo_rtsp_rtcp_sender_info_t parsed_sender;
      turbo_rtsp_rtcp_report_block_t block;
      turbo_rtsp_rtcp_report_block_t parsed_block;
      uint8_t packet[TURBO_RTSP_RTCP_HEADER_SIZE +
                     TURBO_RTSP_RTCP_SENDER_INFO_SIZE +
                     TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE];
      size_t block_count = 0;
      int written = 0;

      memset(&sender, 0, sizeof(sender));
      memset(&parsed_sender, 0, sizeof(parsed_sender));
      memset(&block, 0, sizeof(block));
      memset(&parsed_block, 0, sizeof(parsed_block));

      sender.ssrc = 0x01020304;
      sender.ntp_timestamp = 0x1112131421222324ull;
      sender.rtp_timestamp = 0x31323334;
      sender.packet_count = 5;
      sender.octet_count = 4096;
      block.ssrc = 0x55667788;
      block.fraction_lost = 8;
      block.cumulative_lost = 3;
      block.extended_highest_sequence_number = 0x00010020;
      block.jitter = 0x00000040;
      block.last_sender_report = 0x01020304;
      block.delay_since_last_sender_report = 0x00020000;

      written = turbo_rtsp_rtcp_write_sender_report(
          packet,
          sizeof(packet),
          &sender,
          &block,
          1);
      check_int_eq(written, (int)sizeof(packet));
      check_int_eq(packet[0], 0x81);
      check_int_eq(packet[1], TURBO_RTSP_RTCP_SR);
      check_int_eq(packet[2], 0x00);
      check_int_eq(packet[3], 0x0c);

      check_int_eq(
          turbo_rtsp_rtcp_parse_sender_report(
              packet,
              sizeof(packet),
              &parsed_sender,
              &parsed_block,
              1,
              &block_count),
          0);
      check_size_eq(block_count, 1);
      check_int_eq((int)parsed_sender.ssrc, 0x01020304);
      check(parsed_sender.ntp_timestamp == 0x1112131421222324ull);
      check_int_eq((int)parsed_sender.rtp_timestamp, 0x31323334);
      check_int_eq((int)parsed_block.ssrc, 0x55667788);
      check_int_eq(parsed_block.fraction_lost, 8);
      check_int_eq(parsed_block.cumulative_lost, 3);
    }

    it("formats and parses RTCP SDES CNAME chunks") {
      const uint8_t expected[] = {
          0x81, TURBO_RTSP_RTCP_SDES, 0x00, 0x04,
          0x01, 0x02, 0x03, 0x04,
          TURBO_RTSP_RTCP_SDES_CNAME, 0x08,
          'c', 'a', 'm', 'e', 'r', 'a', '-', '1',
          0x00, 0x00
      };
      uint8_t packet[sizeof(expected)];
      uint32_t ssrc = 0;
      const char *cname = NULL;
      size_t cname_len = 0;

      check_int_eq(
          turbo_rtsp_rtcp_write_sdes_cname(
              packet,
              sizeof(packet),
              0x01020304,
              "camera-1",
              strlen("camera-1")),
          (int)sizeof(expected));
      check_mem_eq(packet, expected, sizeof(expected));

      check_int_eq(
          turbo_rtsp_rtcp_parse_sdes_cname(
              packet,
              sizeof(packet),
              &ssrc,
              &cname,
              &cname_len),
          0);
      check_int_eq((int)ssrc, 0x01020304);
      check_size_eq(cname_len, strlen("camera-1"));
      check_mem_eq(cname, "camera-1", strlen("camera-1"));
    }

    it("formats and parses RTCP BYE packets") {
      const uint8_t expected[] = {
          0x82, TURBO_RTSP_RTCP_BYE, 0x00, 0x04,
          0x01, 0x02, 0x03, 0x04,
          0x11, 0x12, 0x13, 0x14,
          0x04, 'd', 'o', 'n', 'e',
          0x00, 0x00, 0x00
      };
      const uint32_t ssrcs[] = {0x01020304, 0x11121314};
      uint32_t parsed_ssrcs[2];
      uint8_t packet[sizeof(expected)];
      const char *reason = NULL;
      size_t reason_len = 0;
      size_t ssrc_count = 0;

      check_int_eq(
          turbo_rtsp_rtcp_write_bye(
              packet,
              sizeof(packet),
              ssrcs,
              2,
              "done",
              strlen("done")),
          (int)sizeof(expected));
      check_mem_eq(packet, expected, sizeof(expected));

      check_int_eq(
          turbo_rtsp_rtcp_parse_bye(
              packet,
              sizeof(packet),
              parsed_ssrcs,
              2,
              &ssrc_count,
              &reason,
              &reason_len),
          0);
      check_size_eq(ssrc_count, 2);
      check_int_eq((int)parsed_ssrcs[0], 0x01020304);
      check_int_eq((int)parsed_ssrcs[1], 0x11121314);
      check_size_eq(reason_len, strlen("done"));
      check_mem_eq(reason, "done", strlen("done"));

      ssrc_count = 0;
      check_int_eq(
          turbo_rtsp_rtcp_parse_bye(
              packet,
              sizeof(packet),
              parsed_ssrcs,
              1,
              &ssrc_count,
              NULL,
              NULL),
          -1);
      check_size_eq(ssrc_count, 2);
    }

    it("formats and parses RTCP APP packets") {
      const uint8_t data[] = {0xde, 0xad, 0xbe, 0xef};
      const uint8_t expected[] = {
          0x85, TURBO_RTSP_RTCP_APP, 0x00, 0x03,
          0x01, 0x02, 0x03, 0x04,
          'T', 'M', 'E', 'D',
          0xde, 0xad, 0xbe, 0xef
      };
      turbo_rtsp_rtcp_app_t app;
      turbo_rtsp_rtcp_app_t parsed;
      uint8_t packet[sizeof(expected)];

      memset(&app, 0, sizeof(app));
      memset(&parsed, 0, sizeof(parsed));
      app.subtype = 5;
      app.ssrc = 0x01020304;
      memcpy(app.name, "TMED", 4);
      app.data = data;
      app.data_len = sizeof(data);

      check_int_eq(
          turbo_rtsp_rtcp_write_app(packet, sizeof(packet), &app),
          (int)sizeof(expected));
      check_mem_eq(packet, expected, sizeof(expected));

      check_int_eq(turbo_rtsp_rtcp_parse_app(packet, sizeof(packet), &parsed), 0);
      check_int_eq(parsed.subtype, 5);
      check_int_eq((int)parsed.ssrc, 0x01020304);
      check_mem_eq(parsed.name, "TMED", 4);
      check_size_eq(parsed.data_len, sizeof(data));
      check_mem_eq(parsed.data, data, sizeof(data));
    }

    it("formats and parses RTCP RTPFB Generic NACK packets") {
      const uint8_t expected[] = {
          0x81, TURBO_RTSP_RTCP_RTPFB, 0x00, 0x04,
          0x01, 0x02, 0x03, 0x04,
          0x11, 0x12, 0x13, 0x14,
          0x02, 0x77, 0x80, 0x28,
          0x02, 0x88, 0x00, 0x21
      };
      const turbo_rtsp_rtcp_nack_item_t items[] = {
          {631, 0x8028},
          {648, 0x0021}
      };
      turbo_rtsp_rtcp_nack_item_t parsed_items[2];
      uint8_t packet[sizeof(expected)];
      uint32_t sender_ssrc = 0;
      uint32_t media_ssrc = 0;
      size_t item_count = 0;

      memset(parsed_items, 0, sizeof(parsed_items));
      check_int_eq(
          turbo_rtsp_rtcp_write_generic_nack(
              packet,
              sizeof(packet),
              0x01020304,
              0x11121314,
              items,
              sizeof(items) / sizeof(items[0])),
          (int)sizeof(expected));
      check_mem_eq(packet, expected, sizeof(expected));

      check_int_eq(
          turbo_rtsp_rtcp_parse_generic_nack(
              packet,
              sizeof(packet),
              &sender_ssrc,
              &media_ssrc,
              parsed_items,
              sizeof(parsed_items) / sizeof(parsed_items[0]),
              &item_count),
          0);
      check_int_eq((int)sender_ssrc, 0x01020304);
      check_int_eq((int)media_ssrc, 0x11121314);
      check_size_eq(item_count, 2);
      check_int_eq(parsed_items[0].packet_id, 631);
      check_int_eq(parsed_items[0].lost_packet_bitmask, 0x8028);
      check_int_eq(parsed_items[1].packet_id, 648);
      check_int_eq(parsed_items[1].lost_packet_bitmask, 0x0021);
    }

    it("formats and parses RTCP PSFB PLI packets") {
      const uint8_t expected[] = {
          0x81, TURBO_RTSP_RTCP_PSFB, 0x00, 0x02,
          0x01, 0x02, 0x03, 0x04,
          0x11, 0x12, 0x13, 0x14
      };
      uint8_t packet[sizeof(expected)];
      uint32_t sender_ssrc = 0;
      uint32_t media_ssrc = 0;

      check_int_eq(
          turbo_rtsp_rtcp_write_pli(
              packet,
              sizeof(packet),
              0x01020304,
              0x11121314),
          (int)sizeof(expected));
      check_mem_eq(packet, expected, sizeof(expected));

      check_int_eq(
          turbo_rtsp_rtcp_parse_pli(
              packet,
              sizeof(packet),
              &sender_ssrc,
              &media_ssrc),
          0);
      check_int_eq((int)sender_ssrc, 0x01020304);
      check_int_eq((int)media_ssrc, 0x11121314);
    }

    it("rejects invalid RTCP packet headers and report block counters") {
      const uint8_t bad_version[] = {0x40, TURBO_RTSP_RTCP_RR, 0x00, 0x01};
      const uint8_t truncated[] = {0x80, TURBO_RTSP_RTCP_RR, 0x00, 0x02, 0, 0, 0, 0};
      const uint8_t valid_padding[] = {
          0xa0, TURBO_RTSP_RTCP_RR, 0x00, 0x02,
          0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x04
      };
      const uint8_t zero_padding[] = {
          0xa0, TURBO_RTSP_RTCP_RR, 0x00, 0x02,
          0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x00
      };
      const uint8_t oversized_padding[] = {
          0xa0, TURBO_RTSP_RTCP_RR, 0x00, 0x02,
          0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x09
      };
      const uint8_t unaligned_app_data[] = {0xde, 0xad};
      uint8_t report[TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE];
      uint8_t app_packet[16];
      turbo_rtsp_rtcp_header_t header;
      turbo_rtsp_rtcp_report_block_t block;
      turbo_rtsp_rtcp_app_t app;

      memset(&block, 0, sizeof(block));
      memset(&app, 0, sizeof(app));
      block.cumulative_lost = 8388608;
      app.subtype = 32;
      app.ssrc = 0x01020304;
      memcpy(app.name, "BAD!", 4);

      check_int_eq(turbo_rtsp_rtcp_parse_header(bad_version, sizeof(bad_version), &header), -1);
      check_int_eq(turbo_rtsp_rtcp_parse_header(truncated, sizeof(truncated), &header), -1);
      check_int_eq(turbo_rtsp_rtcp_parse_header(valid_padding, sizeof(valid_padding), &header), 0);
      check_int_eq(header.padding, 1);
      check_int_eq(header.padding_len, 4);
      check_size_eq(header.payload_len, 4);
      check_int_eq(turbo_rtsp_rtcp_parse_header(zero_padding, sizeof(zero_padding), &header), -1);
      check_int_eq(
          turbo_rtsp_rtcp_parse_header(oversized_padding, sizeof(oversized_padding), &header),
          -1);
      check_int_eq(turbo_rtsp_rtcp_write_report_block(report, sizeof(report), &block), -1);
      check_int_eq(turbo_rtsp_rtcp_write_app(app_packet, sizeof(app_packet), &app), -1);

      app.subtype = 1;
      app.data = unaligned_app_data;
      app.data_len = sizeof(unaligned_app_data);
      check_int_eq(turbo_rtsp_rtcp_write_app(app_packet, sizeof(app_packet), &app), -1);
      check_int_eq(turbo_rtsp_rtcp_parse_app(truncated, sizeof(truncated), &app), -1);
    }

    it("rejects malformed RTCP feedback packets") {
      const turbo_rtsp_rtcp_nack_item_t item = {10, 0};
      const uint8_t nack_without_fci[] = {
          0x81, TURBO_RTSP_RTCP_RTPFB, 0x00, 0x02,
          0x01, 0x02, 0x03, 0x04,
          0x11, 0x12, 0x13, 0x14
      };
      const uint8_t nack_short_fci[] = {
          0x81, TURBO_RTSP_RTCP_RTPFB, 0x00, 0x03,
          0x01, 0x02, 0x03, 0x04,
          0x11, 0x12, 0x13, 0x14,
          0x00, 0x0a, 0x00, 0x00
      };
      const uint8_t pli_with_fci[] = {
          0x81, TURBO_RTSP_RTCP_PSFB, 0x00, 0x03,
          0x01, 0x02, 0x03, 0x04,
          0x11, 0x12, 0x13, 0x14,
          0x00, 0x00, 0x00, 0x00
      };
      const uint8_t wrong_feedback_type[] = {
          0x82, TURBO_RTSP_RTCP_PSFB, 0x00, 0x02,
          0x01, 0x02, 0x03, 0x04,
          0x11, 0x12, 0x13, 0x14
      };
      turbo_rtsp_rtcp_nack_item_t parsed_item;
      uint8_t packet[20];
      uint32_t sender_ssrc = 0;
      uint32_t media_ssrc = 0;
      size_t item_count = 0;

      check_int_eq(
          turbo_rtsp_rtcp_parse_generic_nack(
              nack_without_fci,
              sizeof(nack_without_fci),
              &sender_ssrc,
              &media_ssrc,
              &parsed_item,
              1,
              &item_count),
          -1);
      check_size_eq(item_count, 0);
      check_int_eq(
          turbo_rtsp_rtcp_parse_generic_nack(
              nack_short_fci,
              sizeof(nack_short_fci),
              &sender_ssrc,
              &media_ssrc,
              NULL,
              0,
              &item_count),
          -1);
      check_size_eq(item_count, 1);
      check_int_eq(
          turbo_rtsp_rtcp_parse_pli(
              pli_with_fci,
              sizeof(pli_with_fci),
              &sender_ssrc,
              &media_ssrc),
          -1);
      check_int_eq(
          turbo_rtsp_rtcp_parse_pli(
              wrong_feedback_type,
              sizeof(wrong_feedback_type),
              &sender_ssrc,
              &media_ssrc),
          -1);
      check_int_eq(
          turbo_rtsp_rtcp_write_generic_nack(
              packet,
              sizeof(packet),
              0x01020304,
              0x11121314,
              NULL,
              1),
          -1);
      check_int_eq(
          turbo_rtsp_rtcp_write_generic_nack(
              packet,
              sizeof(packet),
              0x01020304,
              0x11121314,
              &item,
              0),
          -1);
      check_int_eq(
          turbo_rtsp_rtcp_write_generic_nack(
              packet,
              sizeof(packet) - 1,
              0x01020304,
              0x11121314,
              &item,
              2),
          -1);
      check_int_eq(
          turbo_rtsp_rtcp_write_pli(packet, sizeof(packet) - 9, 0x01020304, 0x11121314),
          -1);
    }

    it("iterates RTCP compound packets with partial detection") {
      uint8_t compound[128];
      uint8_t *cursor = compound;
      size_t remaining = sizeof(compound);
      size_t total_len = 0;
      size_t consumed = 0;
      turbo_rtsp_rtcp_header_t header;
      turbo_rtsp_rtcp_sender_info_t sender;
      turbo_rtsp_rtcp_app_t app;
      const uint32_t ssrcs[] = {0x01020304};
      const uint8_t app_data[] = {0xde, 0xad, 0xbe, 0xef};
      int written = 0;

      memset(&sender, 0, sizeof(sender));
      memset(&app, 0, sizeof(app));
      sender.ssrc = 0x01020304;
      sender.ntp_timestamp = 0x1112131421222324ull;
      sender.rtp_timestamp = 0x31323334;
      sender.packet_count = 5;
      sender.octet_count = 4096;
      app.subtype = 1;
      app.ssrc = 0x01020304;
      memcpy(app.name, "TMED", 4);
      app.data = app_data;
      app.data_len = sizeof(app_data);

      written = turbo_rtsp_rtcp_write_sender_report(cursor, remaining, &sender, NULL, 0);
      check(written > 0);
      cursor += written;
      remaining -= (size_t)written;
      total_len += (size_t)written;

      written = turbo_rtsp_rtcp_write_sdes_cname(
          cursor,
          remaining,
          0x01020304,
          "camera-1",
          strlen("camera-1"));
      check(written > 0);
      cursor += written;
      remaining -= (size_t)written;
      total_len += (size_t)written;

      written = turbo_rtsp_rtcp_write_app(cursor, remaining, &app);
      check(written > 0);
      cursor += written;
      remaining -= (size_t)written;
      total_len += (size_t)written;

      written = turbo_rtsp_rtcp_write_bye(cursor, remaining, ssrcs, 1, NULL, 0);
      check(written > 0);
      total_len += (size_t)written;

      check_int_eq(
          turbo_rtsp_rtcp_next_packet(compound, total_len, &header, &consumed),
          0);
      check_int_eq(header.packet_type, TURBO_RTSP_RTCP_SR);
      check_size_eq(consumed, TURBO_RTSP_RTCP_HEADER_SIZE + TURBO_RTSP_RTCP_SENDER_INFO_SIZE);

      check_int_eq(
          turbo_rtsp_rtcp_next_packet(
              compound + consumed,
              total_len - consumed,
              &header,
              &consumed),
          0);
      check_int_eq(header.packet_type, TURBO_RTSP_RTCP_SDES);
      check_size_eq(consumed, 20);

      check_int_eq(
          turbo_rtsp_rtcp_next_packet(
              compound + TURBO_RTSP_RTCP_HEADER_SIZE + TURBO_RTSP_RTCP_SENDER_INFO_SIZE + 20,
              total_len - TURBO_RTSP_RTCP_HEADER_SIZE - TURBO_RTSP_RTCP_SENDER_INFO_SIZE - 20,
              &header,
              &consumed),
          0);
      check_int_eq(header.packet_type, TURBO_RTSP_RTCP_APP);
      check_size_eq(consumed, 16);

      check_int_eq(
          turbo_rtsp_rtcp_next_packet(
              compound + TURBO_RTSP_RTCP_HEADER_SIZE + TURBO_RTSP_RTCP_SENDER_INFO_SIZE + 20 + 16,
              total_len - TURBO_RTSP_RTCP_HEADER_SIZE - TURBO_RTSP_RTCP_SENDER_INFO_SIZE - 20 - 16,
              &header,
              &consumed),
          0);
      check_int_eq(header.packet_type, TURBO_RTSP_RTCP_BYE);
      check_size_eq(consumed, 8);

      check_int_eq(turbo_rtsp_rtcp_next_packet(compound, 2, &header, &consumed), 1);
      check_size_eq(consumed, 0);
      check_int_eq(
          turbo_rtsp_rtcp_next_packet(
              compound,
              TURBO_RTSP_RTCP_HEADER_SIZE + TURBO_RTSP_RTCP_SENDER_INFO_SIZE - 1,
              &header,
              &consumed),
          1);
      check_size_eq(consumed, 0);

      compound[0] = 0x40;
      check_int_eq(
          turbo_rtsp_rtcp_next_packet(compound, TURBO_RTSP_RTCP_HEADER_SIZE, &header, &consumed),
          -1);
    }

    it("validates RFC 3550 RTCP compound packet shape") {
      uint8_t compound[128];
      uint8_t mismatched_cname[128];
      uint8_t missing_cname[64];
      uint8_t padded_compound[128];
      uint8_t *cursor = compound;
      size_t remaining = sizeof(compound);
      size_t total_len = 0;
      size_t sr_len = 0;
      size_t sdes_len = 0;
      size_t packet_count = 0;
      turbo_rtsp_rtcp_sender_info_t sender;
      const uint32_t ssrcs[] = {0x01020304};
      int written = 0;

      memset(&sender, 0, sizeof(sender));
      sender.ssrc = 0x01020304;
      sender.ntp_timestamp = 0x1112131421222324ull;
      sender.rtp_timestamp = 0x31323334;
      sender.packet_count = 5;
      sender.octet_count = 4096;

      written = turbo_rtsp_rtcp_write_sender_report(cursor, remaining, &sender, NULL, 0);
      check(written > 0);
      sr_len = (size_t)written;
      cursor += written;
      remaining -= (size_t)written;
      total_len += (size_t)written;

      written = turbo_rtsp_rtcp_write_sdes_cname(
          cursor,
          remaining,
          0x01020304,
          "camera-1",
          strlen("camera-1"));
      check(written > 0);
      sdes_len = (size_t)written;
      cursor += written;
      remaining -= (size_t)written;
      total_len += (size_t)written;

      written = turbo_rtsp_rtcp_write_bye(cursor, remaining, ssrcs, 1, NULL, 0);
      check(written > 0);
      total_len += (size_t)written;

      check_int_eq(turbo_rtsp_rtcp_validate_compound(compound, total_len, &packet_count), 0);
      check_size_eq(packet_count, 3);

      memcpy(mismatched_cname, compound, total_len);
      mismatched_cname[sr_len + TURBO_RTSP_RTCP_HEADER_SIZE + 0] = 0x90;
      mismatched_cname[sr_len + TURBO_RTSP_RTCP_HEADER_SIZE + 1] = 0x90;
      mismatched_cname[sr_len + TURBO_RTSP_RTCP_HEADER_SIZE + 2] = 0x90;
      mismatched_cname[sr_len + TURBO_RTSP_RTCP_HEADER_SIZE + 3] = 0x90;
      check_int_eq(turbo_rtsp_rtcp_validate_compound(mismatched_cname, total_len, NULL), -1);

      memcpy(padded_compound, compound, sr_len + sdes_len);
      padded_compound[sr_len + sdes_len + 0] = 0xa1;
      padded_compound[sr_len + sdes_len + 1] = TURBO_RTSP_RTCP_BYE;
      padded_compound[sr_len + sdes_len + 2] = 0x00;
      padded_compound[sr_len + sdes_len + 3] = 0x02;
      padded_compound[sr_len + sdes_len + 4] = 0x01;
      padded_compound[sr_len + sdes_len + 5] = 0x02;
      padded_compound[sr_len + sdes_len + 6] = 0x03;
      padded_compound[sr_len + sdes_len + 7] = 0x04;
      padded_compound[sr_len + sdes_len + 8] = 0x00;
      padded_compound[sr_len + sdes_len + 9] = 0x00;
      padded_compound[sr_len + sdes_len + 10] = 0x00;
      padded_compound[sr_len + sdes_len + 11] = 0x04;
      packet_count = 0;
      check_int_eq(
          turbo_rtsp_rtcp_validate_compound(
              padded_compound,
              sr_len + sdes_len + 12u,
              &packet_count),
          0);
      check_size_eq(packet_count, 3);

      check_int_eq(
          turbo_rtsp_rtcp_validate_compound(compound + sr_len, total_len - sr_len, NULL),
          -1);

      memcpy(missing_cname, compound, sr_len);
      written = turbo_rtsp_rtcp_write_bye(
          missing_cname + sr_len,
          sizeof(missing_cname) - sr_len,
          ssrcs,
          1,
          NULL,
          0);
      check(written > 0);
      check_int_eq(
          turbo_rtsp_rtcp_validate_compound(
              missing_cname,
              sr_len + (size_t)written,
              NULL),
          -1);

      compound[0] |= 0x20;
      check_int_eq(turbo_rtsp_rtcp_validate_compound(compound, total_len, NULL), -1);
      compound[0] = (uint8_t)(compound[0] & 0xdfu);

      UNUSED(sdes_len);
    }

    it("converts RTCP NTP timestamps and delays into LSR and DLSR fields") {
      check_int_eq(
          (int)turbo_rtsp_rtcp_ntp_to_lsr(0x83aa7e8000000000ull),
          0x7e800000);
      check_int_eq(
          (int)turbo_rtsp_rtcp_ntp_to_lsr(0x1112131421222324ull),
          0x13142122);
      check_int_eq((int)turbo_rtsp_rtcp_delay_us_to_dlsr(1000000), 65536);
      check_int_eq((int)turbo_rtsp_rtcp_delay_us_to_dlsr(250000), 16384);
      check_int_eq((int)turbo_rtsp_rtcp_delay_us_to_dlsr(UINT64_MAX), -1);
    }

    it("tracks RTP sequence loss, wrap and jitter for RTCP reports") {
      turbo_rtsp_rtp_source_t source;
      turbo_rtsp_rtp_header_t header;
      turbo_rtsp_rtcp_report_block_t report;

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_source_init(&source, 0);

      header.sequence_number = 65534;
      header.timestamp = 1000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 1000), 0);

      header.sequence_number = 65535;
      header.timestamp = 2000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 2020), 0);

      header.sequence_number = 1;
      header.timestamp = 4000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 4050), 0);

      check_int_eq(
          turbo_rtsp_rtp_source_make_report(
              &source,
              0x11121314,
              0x00010000,
              &report),
          0);
      check_int_eq((int)report.ssrc, 0x01020304);
      check_int_eq(report.fraction_lost, 64);
      check_int_eq(report.cumulative_lost, 1);
      check_int_eq((int)report.extended_highest_sequence_number, 65537);
      check_int_eq((int)report.jitter, 3);
      check_int_eq((int)report.last_sender_report, 0x11121314);
      check_int_eq((int)report.delay_since_last_sender_report, 0x00010000);

      header.sequence_number = 2;
      header.timestamp = 5000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 5060), 0);
      check_int_eq(turbo_rtsp_rtp_source_make_report(&source, 0, 0, &report), 0);
      check_int_eq(report.fraction_lost, 0);
      check_int_eq(report.cumulative_lost, 1);
      check_int_eq((int)report.extended_highest_sequence_number, 65538);

      header.ssrc = 0x10203040;
      header.sequence_number = 3;
      header.timestamp = 6000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 6060), -1);
    }

    it("classifies duplicate RTP source updates without counting them twice") {
      turbo_rtsp_rtp_source_t source;
      turbo_rtsp_rtp_header_t header;

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_source_init(&source, 0);

      header.sequence_number = 10;
      header.timestamp = 1000;
      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 1000),
          TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check_int_eq((int)source.received, 1);

      header.timestamp = 2000;
      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 2000),
          TURBO_RTSP_RTP_SOURCE_UPDATE_DUPLICATE);
      check_int_eq((int)source.received, 1);

      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 2000), 0);
      check_int_eq((int)source.received, 1);
    }

    it("classifies slight RTP source reordering as late or out of order") {
      turbo_rtsp_rtp_source_t source;
      turbo_rtsp_rtp_header_t header;

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_source_init(&source, 0);

      header.sequence_number = 10;
      header.timestamp = 1000;
      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 1000),
          TURBO_RTSP_RTP_SOURCE_UPDATE_OK);

      header.sequence_number = 12;
      header.timestamp = 3000;
      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 3020),
          TURBO_RTSP_RTP_SOURCE_UPDATE_OK);

      header.sequence_number = 11;
      header.timestamp = 2000;
      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 2030),
          TURBO_RTSP_RTP_SOURCE_UPDATE_LATE_OR_OUT_OF_ORDER);
      check_int_eq((int)source.received, 3);
      check_int_eq(source.max_sequence_number, 12);

      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 2030),
          TURBO_RTSP_RTP_SOURCE_UPDATE_DUPLICATE);
      check_int_eq((int)source.received, 3);
    }

    it("classifies large RTP source sequence jumps as dropout") {
      turbo_rtsp_rtp_source_t source;
      turbo_rtsp_rtp_header_t header;

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_source_init(&source, 0);

      header.sequence_number = 10;
      header.timestamp = 1000;
      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 1000),
          TURBO_RTSP_RTP_SOURCE_UPDATE_OK);

      header.sequence_number = 4011;
      header.timestamp = 2000;
      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 2020),
          TURBO_RTSP_RTP_SOURCE_UPDATE_DROPOUT);
      check_int_eq((int)source.received, 2);
      check_int_eq(source.max_sequence_number, 4011);
    }

    it("classifies RTP source SSRC mismatches") {
      turbo_rtsp_rtp_source_t source;
      turbo_rtsp_rtp_header_t header;

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.sequence_number = 1;
      header.timestamp = 1000;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_source_init(&source, 0x10203040);
      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 1000),
          TURBO_RTSP_RTP_SOURCE_UPDATE_SSRC_MISMATCH);
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 1000), -1);

      turbo_rtsp_rtp_source_init(&source, 0);
      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 1000),
          TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      header.ssrc = 0x10203040;
      header.sequence_number = 2;
      check_int_eq(
          turbo_rtsp_rtp_source_update_ex(&source, &header, 2000),
          TURBO_RTSP_RTP_SOURCE_UPDATE_SSRC_MISMATCH);
      check_int_eq((int)source.received, 1);
    }

    it("writes receiver reports directly from RTP source state") {
      turbo_rtsp_rtp_source_t source;
      turbo_rtsp_rtp_header_t header;
      turbo_rtsp_rtcp_report_block_t block;
      uint8_t packet[TURBO_RTSP_RTCP_HEADER_SIZE + 4 + TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE];
      uint32_t reporter_ssrc = 0;
      size_t block_count = 0;
      int written = 0;

      memset(&header, 0, sizeof(header));
      memset(&block, 0, sizeof(block));
      header.version = 2;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_source_init(&source, 0);

      header.sequence_number = 10;
      header.timestamp = 1000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 1000), 0);

      header.sequence_number = 12;
      header.timestamp = 3000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 3030), 0);

      written = turbo_rtsp_rtp_source_write_receiver_report(
          &source,
          packet,
          sizeof(packet),
          0x0a0b0c0d,
          turbo_rtsp_rtcp_ntp_to_lsr(0x1112131421222324ull),
          turbo_rtsp_rtcp_delay_us_to_dlsr(1000000));
      check_int_eq(written, (int)sizeof(packet));
      check_int_eq(
          turbo_rtsp_rtcp_parse_receiver_report(
              packet,
              (size_t)written,
              &reporter_ssrc,
              &block,
              1,
              &block_count),
          0);
      check_int_eq((int)reporter_ssrc, 0x0a0b0c0d);
      check_size_eq(block_count, 1);
      check_int_eq((int)block.ssrc, 0x01020304);
      check_int_eq(block.fraction_lost, 85);
      check_int_eq(block.cumulative_lost, 1);
      check_int_eq((int)block.extended_highest_sequence_number, 12);
      check_int_eq((int)block.last_sender_report, 0x13142122);
      check_int_eq((int)block.delay_since_last_sender_report, 0x00010000);

      header.sequence_number = 13;
      header.timestamp = 4000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 4040), 0);
      written = turbo_rtsp_rtp_source_write_receiver_report(
          &source,
          packet,
          sizeof(packet),
          0x0a0b0c0d,
          0,
          0);
      check_int_eq(written, (int)sizeof(packet));
      check_int_eq(
          turbo_rtsp_rtcp_parse_receiver_report(
              packet,
              (size_t)written,
              &reporter_ssrc,
              &block,
              1,
              &block_count),
          0);
      check_int_eq(block.fraction_lost, 0);
      check_int_eq(block.cumulative_lost, 1);
      check_int_eq((int)block.extended_highest_sequence_number, 13);
    }

    it("writes RTCP RR and SDES CNAME compound packets from RTP source state") {
      turbo_rtsp_rtp_source_t source;
      turbo_rtsp_rtp_header_t header;
      turbo_rtsp_rtcp_header_t rtcp_header;
      turbo_rtsp_rtcp_report_block_t block;
      uint8_t packet[64];
      uint32_t reporter_ssrc = 0;
      uint32_t sdes_ssrc = 0;
      const char *cname = NULL;
      size_t cname_len = 0;
      size_t block_count = 0;
      size_t consumed = 0;
      size_t packet_count = 0;
      int written = 0;

      memset(&header, 0, sizeof(header));
      memset(&block, 0, sizeof(block));
      header.version = 2;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_source_init(&source, 0);
      header.sequence_number = 10;
      header.timestamp = 1000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 1000), 0);
      header.sequence_number = 12;
      header.timestamp = 3000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 3030), 0);

      written = turbo_rtsp_rtp_source_write_rtcp_compound(
          &source,
          packet,
          sizeof(packet),
          0x0a0b0c0d,
          0x11121314,
          0x00010000,
          "receiver",
          strlen("receiver"));
      check_int_eq(written, 52);
      check_int_eq(turbo_rtsp_rtcp_validate_compound(packet, (size_t)written, &packet_count), 0);
      check_size_eq(packet_count, 2);

      check_int_eq(
          turbo_rtsp_rtcp_next_packet(packet, (size_t)written, &rtcp_header, &consumed),
          0);
      check_int_eq(rtcp_header.packet_type, TURBO_RTSP_RTCP_RR);
      check_size_eq(consumed, TURBO_RTSP_RTCP_HEADER_SIZE + 4 + TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE);
      check_int_eq(
          turbo_rtsp_rtcp_parse_receiver_report(
              packet,
              consumed,
              &reporter_ssrc,
              &block,
              1,
              &block_count),
          0);
      check_int_eq((int)reporter_ssrc, 0x0a0b0c0d);
      check_size_eq(block_count, 1);
      check_int_eq((int)block.ssrc, 0x01020304);
      check_int_eq(block.fraction_lost, 85);
      check_int_eq(block.cumulative_lost, 1);

      check_int_eq(
          turbo_rtsp_rtcp_parse_sdes_cname(
              packet + consumed,
              (size_t)written - consumed,
              &sdes_ssrc,
              &cname,
              &cname_len),
          0);
      check_int_eq((int)sdes_ssrc, 0x0a0b0c0d);
      check_size_eq(cname_len, strlen("receiver"));
      check_mem_eq(cname, "receiver", strlen("receiver"));
    }

    it("writes sender reports directly from RTP sender state") {
      turbo_rtsp_rtp_sender_t sender;
      turbo_rtsp_rtp_header_t header;
      turbo_rtsp_rtcp_sender_info_t parsed;
      uint8_t packet[TURBO_RTSP_RTCP_HEADER_SIZE + TURBO_RTSP_RTCP_SENDER_INFO_SIZE];
      size_t block_count = 0;
      int written = 0;

      memset(&header, 0, sizeof(header));
      memset(&parsed, 0, sizeof(parsed));
      header.version = 2;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_sender_init(&sender, 0);
      header.payload_len = 160;
      check_int_eq(turbo_rtsp_rtp_sender_update(&sender, &header), 0);
      header.payload_len = 1200;
      check_int_eq(turbo_rtsp_rtp_sender_update(&sender, &header), 0);

      written = turbo_rtsp_rtp_sender_write_sender_report(
          &sender,
          packet,
          sizeof(packet),
          0x1112131421222324ull,
          0x31323334,
          NULL,
          0);
      check_int_eq(written, (int)sizeof(packet));
      check_int_eq(
          turbo_rtsp_rtcp_parse_sender_report(
              packet,
              (size_t)written,
              &parsed,
              NULL,
              0,
              &block_count),
          0);
      check_size_eq(block_count, 0);
      check_int_eq((int)parsed.ssrc, 0x01020304);
      check(parsed.ntp_timestamp == 0x1112131421222324ull);
      check_int_eq((int)parsed.rtp_timestamp, 0x31323334);
      check_int_eq((int)parsed.packet_count, 2);
      check_int_eq((int)parsed.octet_count, 1360);

      header.ssrc = 0x10203040;
      header.payload_len = 1;
      check_int_eq(turbo_rtsp_rtp_sender_update(&sender, &header), -1);

      turbo_rtsp_rtp_sender_init(&sender, 0x10203040);
      header.ssrc = 0x01020304;
      check_int_eq(turbo_rtsp_rtp_sender_update(&sender, &header), -1);
    }

    it("writes RTCP SR and SDES CNAME compound packets from RTP sender state") {
      turbo_rtsp_rtp_sender_t sender;
      turbo_rtsp_rtp_header_t header;
      turbo_rtsp_rtcp_header_t rtcp_header;
      turbo_rtsp_rtcp_sender_info_t parsed;
      uint8_t packet[64];
      uint32_t sdes_ssrc = 0;
      const char *cname = NULL;
      size_t cname_len = 0;
      size_t block_count = 0;
      size_t consumed = 0;
      size_t packet_count = 0;
      int written = 0;

      memset(&header, 0, sizeof(header));
      memset(&parsed, 0, sizeof(parsed));
      header.version = 2;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_sender_init(&sender, 0);
      header.payload_len = 160;
      check_int_eq(turbo_rtsp_rtp_sender_update(&sender, &header), 0);
      header.payload_len = 1200;
      check_int_eq(turbo_rtsp_rtp_sender_update(&sender, &header), 0);

      written = turbo_rtsp_rtp_sender_write_rtcp_compound(
          &sender,
          packet,
          sizeof(packet),
          0x1112131421222324ull,
          0x31323334,
          NULL,
          0,
          "sender-1",
          strlen("sender-1"));
      check_int_eq(written, 48);
      check_int_eq(turbo_rtsp_rtcp_validate_compound(packet, (size_t)written, &packet_count), 0);
      check_size_eq(packet_count, 2);

      check_int_eq(
          turbo_rtsp_rtcp_next_packet(packet, (size_t)written, &rtcp_header, &consumed),
          0);
      check_int_eq(rtcp_header.packet_type, TURBO_RTSP_RTCP_SR);
      check_size_eq(consumed, TURBO_RTSP_RTCP_HEADER_SIZE + TURBO_RTSP_RTCP_SENDER_INFO_SIZE);
      check_int_eq(
          turbo_rtsp_rtcp_parse_sender_report(
              packet,
              consumed,
              &parsed,
              NULL,
              0,
              &block_count),
          0);
      check_size_eq(block_count, 0);
      check_int_eq((int)parsed.ssrc, 0x01020304);
      check(parsed.ntp_timestamp == 0x1112131421222324ull);
      check_int_eq((int)parsed.rtp_timestamp, 0x31323334);
      check_int_eq((int)parsed.packet_count, 2);
      check_int_eq((int)parsed.octet_count, 1360);

      check_int_eq(
          turbo_rtsp_rtcp_parse_sdes_cname(
              packet + consumed,
              (size_t)written - consumed,
              &sdes_ssrc,
              &cname,
              &cname_len),
          0);
      check_int_eq((int)sdes_ssrc, 0x01020304);
      check_size_eq(cname_len, strlen("sender-1"));
      check_mem_eq(cname, "sender-1", strlen("sender-1"));
    }

    it("rejects invalid RTCP compound CNAME and buffer inputs") {
      turbo_rtsp_rtp_source_t source;
      turbo_rtsp_rtp_sender_t sender;
      turbo_rtsp_rtp_header_t header;
      uint8_t packet[52];
      char long_cname[256];

      memset(&header, 0, sizeof(header));
      memset(long_cname, 'x', sizeof(long_cname));
      header.version = 2;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_source_init(&source, 0);
      header.sequence_number = 10;
      header.timestamp = 1000;
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 1000), 0);

      check_int_eq(
          turbo_rtsp_rtp_source_write_rtcp_compound(
              &source, packet, sizeof(packet), 0x0a0b0c0d, 0, 0, NULL, 8),
          -1);
      check_int_eq(
          turbo_rtsp_rtp_source_write_rtcp_compound(
              &source, packet, sizeof(packet), 0x0a0b0c0d, 0, 0, "", 0),
          -1);
      check_int_eq(
          turbo_rtsp_rtp_source_write_rtcp_compound(
              &source, packet, sizeof(packet), 0x0a0b0c0d, 0, 0, long_cname, sizeof(long_cname)),
          -1);
      check_int_eq(
          turbo_rtsp_rtp_source_write_rtcp_compound(
              &source, packet, sizeof(packet) - 1, 0x0a0b0c0d, 0, 0, "receiver", strlen("receiver")),
          -1);
      check_int_eq((int)source.expected_prior, 0);

      turbo_rtsp_rtp_sender_init(&sender, 0);
      header.payload_len = 160;
      check_int_eq(turbo_rtsp_rtp_sender_update(&sender, &header), 0);
      check_int_eq(
          turbo_rtsp_rtp_sender_write_rtcp_compound(
              &sender, packet, sizeof(packet), 0, 0, NULL, 0, NULL, 8),
          -1);
      check_int_eq(
          turbo_rtsp_rtp_sender_write_rtcp_compound(
              &sender, packet, sizeof(packet), 0, 0, NULL, 0, long_cname, sizeof(long_cname)),
          -1);
      check_int_eq(
          turbo_rtsp_rtp_sender_write_rtcp_compound(
              &sender, packet, 47, 0, 0, NULL, 0, "sender-1", strlen("sender-1")),
          -1);
    }

    it("writes raw RTP stream payloads and advances sender state") {
      static const uint8_t audio[] = {0x55, 0x56, 0x57, 0x58};
      static uint8_t packet[32];
      static turbo_rtsp_rtp_stream_t stream;
      static turbo_rtsp_rtp_header_t parsed;
      size_t packet_len = 99;
      size_t header_len = 0;

      memset(&stream, 0, sizeof(stream));
      memset(packet, 0, sizeof(packet));
      memset(&parsed, 0, sizeof(parsed));
      turbo_rtsp_rtp_stream_init(&stream, 0, 0x01020304, 77, 8000, 8000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_payload(
              &stream,
              audio,
              sizeof(audio),
              1,
              sizeof(audio),
              packet,
              sizeof(packet),
              &packet_len),
          0);

      check_size_eq(packet_len, TURBO_RTSP_RTP_HEADER_SIZE + sizeof(audio));
      check_int_eq(
          turbo_rtsp_rtp_parse_header(
              packet,
              packet_len,
              &parsed,
              &header_len),
          0);
      check_size_eq(header_len, TURBO_RTSP_RTP_HEADER_SIZE);
      check_int_eq(parsed.marker, 1);
      check_int_eq(parsed.payload_type, 0);
      check_int_eq(parsed.sequence_number, 77);
      check_int_eq((int)parsed.timestamp, 8000);
      check_int_eq((int)parsed.ssrc, 0x01020304);
      check_size_eq(parsed.payload_len, sizeof(audio));
      check_mem_eq(parsed.payload, audio, sizeof(audio));
      check_int_eq(stream.sequence_number, 78);
      check_int_eq((int)stream.timestamp, 8004);
      check_int_eq((int)stream.sender.packet_count, 1);
      check_int_eq((int)stream.sender.octet_count, 4);
    }

    it("does not advance RTP stream state when raw payload output buffer is too small") {
      static const uint8_t audio[] = {0xd5, 0xd6, 0xd7, 0xd8};
      static uint8_t packet[TURBO_RTSP_RTP_HEADER_SIZE + 3];
      static turbo_rtsp_rtp_stream_t stream;
      size_t packet_len = 99;

      memset(&stream, 0, sizeof(stream));
      memset(packet, 0, sizeof(packet));
      turbo_rtsp_rtp_stream_init(&stream, 8, 0x01020304, 10, 1000, 8000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_payload(
              &stream,
              audio,
              sizeof(audio),
              0,
              sizeof(audio),
              packet,
              sizeof(packet),
              &packet_len),
          -1);
      check_size_eq(packet_len, 0);
      check_int_eq(stream.sequence_number, 10);
      check_int_eq((int)stream.timestamp, 1000);
      check_int_eq((int)stream.sender.packet_count, 0);
      check_int_eq((int)stream.sender.octet_count, 0);
    }

    it("rejects invalid raw RTP stream inputs without advancing state") {
      static const uint8_t audio[] = {0x11};
      static uint8_t packet[32];
      static turbo_rtsp_rtp_stream_t stream;
      size_t packet_len = 99;

      memset(&stream, 0, sizeof(stream));
      memset(packet, 0, sizeof(packet));
      turbo_rtsp_rtp_stream_init(&stream, 96, 0x01020304, 20, 2000, 8000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_payload(
              &stream,
              audio,
              0,
              0,
              1,
              packet,
              sizeof(packet),
              &packet_len),
          -1);
      check_size_eq(packet_len, 0);

      stream.payload_type = 128;
      packet_len = 99;
      check_int_eq(
          turbo_rtsp_rtp_stream_write_payload(
              &stream,
              audio,
              sizeof(audio),
              0,
              1,
              packet,
              sizeof(packet),
              &packet_len),
          -1);
      check_size_eq(packet_len, 0);
      check_int_eq(stream.sequence_number, 20);
      check_int_eq((int)stream.timestamp, 2000);
      check_int_eq((int)stream.sender.packet_count, 0);
      check_int_eq((int)stream.sender.octet_count, 0);
    }

    it("writes MPEG4-GENERIC AAC-hbr AU RTP stream packets") {
      static const uint8_t au[] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8};
      static const uint8_t expected_au_header[] = {0x00, 0x10, 0x01, 0x48};
      static const uint8_t expected_fragment_lens[] = {4, 4, 1};
      static turbo_rtsp_rtp_stream_packet_t packets[3];
      static uint8_t packet[3 * 20];
      static turbo_rtsp_rtp_stream_t stream;
      size_t packet_count = 0;
      size_t offset = 0;
      size_t i = 0;

      memset(&stream, 0, sizeof(stream));
      memset(packet, 0, sizeof(packet));
      memset(packets, 0, sizeof(packets));
      turbo_rtsp_rtp_stream_init(&stream, 97, 0x01020304, 500, 48000, 48000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_mpeg4_generic_au(
              &stream,
              au,
              sizeof(au),
              1024,
              8,
              packet,
              20,
              packets,
              3,
              &packet_count),
          0);
      check_size_eq(packet_count, 3);

      for (i = 0; i < packet_count; ++i) {
        turbo_rtsp_rtp_header_t parsed;
        size_t header_len = 0;
        size_t fragment_len = expected_fragment_lens[i];

        memset(&parsed, 0, sizeof(parsed));
        check(packets[i].packet == packet + (i * 20));
        check_size_eq(
            packets[i].packet_len,
            TURBO_RTSP_RTP_HEADER_SIZE +
                TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE + fragment_len);
        check_int_eq(
            turbo_rtsp_rtp_parse_header(
                packets[i].packet,
                packets[i].packet_len,
                &parsed,
                &header_len),
            0);
        check_size_eq(header_len, TURBO_RTSP_RTP_HEADER_SIZE);
        check_int_eq(parsed.marker, i == 2 ? 1 : 0);
        check_int_eq(parsed.payload_type, 97);
        check_int_eq(parsed.sequence_number, (uint16_t)(500 + i));
        check_int_eq((int)parsed.timestamp, 48000);
        check_int_eq((int)parsed.ssrc, 0x01020304);
        check_size_eq(
            parsed.payload_len,
            TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE + fragment_len);
        check_mem_eq(
            parsed.payload,
            expected_au_header,
            TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE);
        check_mem_eq(
            parsed.payload + TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE,
            au + offset,
            fragment_len);
        offset += fragment_len;
      }

      check_int_eq(stream.sequence_number, 503);
      check_int_eq((int)stream.timestamp, 49024);
      check_int_eq((int)stream.sender.packet_count, 3);
      check_int_eq((int)stream.sender.octet_count, 21);
    }

    it("does not advance MPEG4-GENERIC RTP stream state on invalid output") {
      static uint8_t au[] = {0x55};
      static turbo_rtsp_rtp_stream_packet_t packets[1];
      static uint8_t packet[32];
      static turbo_rtsp_rtp_stream_t stream;
      size_t packet_count = 99;

      memset(&stream, 0, sizeof(stream));
      memset(packet, 0, sizeof(packet));
      memset(packets, 0, sizeof(packets));
      turbo_rtsp_rtp_stream_init(&stream, 97, 0x01020304, 20, 2000, 48000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_mpeg4_generic_au(
              &stream,
              au,
              TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_MAX_AU_SIZE + 1,
              1024,
              8,
              packet,
              20,
              packets,
              1,
              &packet_count),
          -1);
      check_size_eq(packet_count, 0);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_mpeg4_generic_au(
              &stream,
              au,
              4,
              1024,
              TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE,
              packet,
              sizeof(packet),
              packets,
              1,
              &packet_count),
          -1);
      check_int_eq(stream.sequence_number, 20);
      check_int_eq((int)stream.timestamp, 2000);
      check_int_eq((int)stream.sender.packet_count, 0);
      check_int_eq((int)stream.sender.octet_count, 0);
    }

    it("writes MP4A-LATM RTP stream packets") {
      static const uint8_t latm[] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9};
      static const size_t expected_payload_lens[] = {5, 5, 1};
      static turbo_rtsp_rtp_stream_packet_t packets[3];
      static uint8_t packet[3 * 20];
      static turbo_rtsp_rtp_stream_t stream;
      size_t packet_count = 0;
      size_t offset = 0;
      size_t i = 0;

      memset(&stream, 0, sizeof(stream));
      memset(packet, 0, sizeof(packet));
      memset(packets, 0, sizeof(packets));
      turbo_rtsp_rtp_stream_init(&stream, 96, 0x01020304, 600, 48000, 48000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_mp4a_latm(
              &stream,
              latm,
              sizeof(latm),
              1024,
              5,
              packet,
              20,
              packets,
              3,
              &packet_count),
          0);
      check_size_eq(packet_count, 3);

      for (i = 0; i < packet_count; ++i) {
        turbo_rtsp_rtp_header_t parsed;
        size_t header_len = 0;

        memset(&parsed, 0, sizeof(parsed));
        check_size_eq(
            packets[i].packet_len,
            TURBO_RTSP_RTP_HEADER_SIZE + expected_payload_lens[i]);
        check_int_eq(
            turbo_rtsp_rtp_parse_header(
                packets[i].packet,
                packets[i].packet_len,
                &parsed,
                &header_len),
            0);
        check_size_eq(header_len, TURBO_RTSP_RTP_HEADER_SIZE);
        check_int_eq(parsed.marker, i == 2 ? 1 : 0);
        check_int_eq(parsed.payload_type, 96);
        check_int_eq(parsed.sequence_number, (uint16_t)(600 + i));
        check_int_eq((int)parsed.timestamp, 48000);
        check_int_eq((int)parsed.ssrc, 0x01020304);
        check_size_eq(parsed.payload_len, expected_payload_lens[i]);
        if (i == 0) {
          check_int_eq(parsed.payload[0], (int)sizeof(latm));
          check_mem_eq(parsed.payload + 1, latm, 4);
          offset = 4;
        } else {
          check_mem_eq(parsed.payload, latm + offset, expected_payload_lens[i]);
          offset += expected_payload_lens[i];
        }
      }

      check_int_eq(stream.sequence_number, 603);
      check_int_eq((int)stream.timestamp, 49024);
      check_int_eq((int)stream.sender.packet_count, 3);
      check_int_eq((int)stream.sender.octet_count, 11);
    }

    it("does not advance MP4A-LATM RTP stream state on invalid output") {
      static uint8_t latm[] = {0x55};
      static turbo_rtsp_rtp_stream_packet_t packets[1];
      static uint8_t packet[16];
      static turbo_rtsp_rtp_stream_t stream;
      size_t packet_count = 99;

      memset(&stream, 0, sizeof(stream));
      memset(packet, 0, sizeof(packet));
      memset(packets, 0, sizeof(packets));
      turbo_rtsp_rtp_stream_init(&stream, 96, 0x01020304, 20, 2000, 48000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_mp4a_latm(
              &stream,
              latm,
              sizeof(latm),
              1024,
              1,
              packet,
              sizeof(packet),
              packets,
              1,
              &packet_count),
          -1);
      check_size_eq(packet_count, 0);
      check_int_eq(stream.sequence_number, 20);
      check_int_eq((int)stream.timestamp, 2000);
      check_int_eq((int)stream.sender.packet_count, 0);
      check_int_eq((int)stream.sender.octet_count, 0);
    }

    it("writes MPEG2 TS RTP stream packets on TS packet boundaries") {
      static uint8_t ts_data[TURBO_RTSP_MPEG2_TS_PACKET_SIZE * 5];
      static turbo_rtsp_rtp_stream_packet_t packets[3];
      static uint8_t packet[3 * (TURBO_RTSP_RTP_HEADER_SIZE + TURBO_RTSP_MPEG2_TS_PACKET_SIZE * 2)];
      static turbo_rtsp_rtp_stream_t stream;
      static const size_t expected_payload_lens[] = {
          TURBO_RTSP_MPEG2_TS_PACKET_SIZE * 2,
          TURBO_RTSP_MPEG2_TS_PACKET_SIZE * 2,
          TURBO_RTSP_MPEG2_TS_PACKET_SIZE
      };
      size_t packet_count = 0;
      size_t offset = 0;
      size_t i = 0;

      turbo_rtsp_test_fill_mpeg2_ts(ts_data, sizeof(ts_data));
      memset(packet, 0, sizeof(packet));
      memset(packets, 0, sizeof(packets));
      memset(&stream, 0, sizeof(stream));
      turbo_rtsp_rtp_stream_init(&stream, 33, 0x01020304, 900, 90000, 90000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_mpeg2_ts(
              &stream,
              ts_data,
              sizeof(ts_data),
              3600,
              TURBO_RTSP_MPEG2_TS_PACKET_SIZE * 2 + 10,
              packet,
              TURBO_RTSP_RTP_HEADER_SIZE + TURBO_RTSP_MPEG2_TS_PACKET_SIZE * 2,
              packets,
              3,
              &packet_count),
          0);
      check_size_eq(packet_count, 3);

      for (i = 0; i < packet_count; ++i) {
        turbo_rtsp_rtp_header_t parsed;
        turbo_rtsp_mpeg2_ts_payload_t ts;
        size_t header_len = 0;

        memset(&parsed, 0, sizeof(parsed));
        memset(&ts, 0, sizeof(ts));
        check_size_eq(
            packets[i].packet_len,
            TURBO_RTSP_RTP_HEADER_SIZE + expected_payload_lens[i]);
        check_int_eq(
            turbo_rtsp_rtp_parse_header(
                packets[i].packet,
                packets[i].packet_len,
                &parsed,
                &header_len),
            0);
        check_size_eq(header_len, TURBO_RTSP_RTP_HEADER_SIZE);
        check_int_eq(parsed.marker, 0);
        check_int_eq(parsed.payload_type, 33);
        check_int_eq(parsed.sequence_number, (uint16_t)(900 + i));
        check_int_eq((int)parsed.timestamp, 90000);
        check_int_eq((int)parsed.ssrc, 0x01020304);
        check_size_eq(parsed.payload_len, expected_payload_lens[i]);
        check_mem_eq(parsed.payload, ts_data + offset, expected_payload_lens[i]);
        check_int_eq(
            turbo_rtsp_mpeg2_ts_payload_parse(parsed.payload, parsed.payload_len, &ts),
            0);
        offset += expected_payload_lens[i];
      }

      check_int_eq(stream.sequence_number, 903);
      check_int_eq((int)stream.timestamp, 93600);
      check_int_eq((int)stream.sender.packet_count, 3);
      check_int_eq((int)stream.sender.octet_count, (int)sizeof(ts_data));
    }

    it("does not advance MPEG2 TS RTP stream state on invalid output") {
      static uint8_t ts_data[TURBO_RTSP_MPEG2_TS_PACKET_SIZE];
      static turbo_rtsp_rtp_stream_packet_t packets[1];
      static uint8_t packet[TURBO_RTSP_RTP_HEADER_SIZE + TURBO_RTSP_MPEG2_TS_PACKET_SIZE - 1];
      static turbo_rtsp_rtp_stream_t stream;
      size_t packet_count = 99;

      turbo_rtsp_test_fill_mpeg2_ts(ts_data, sizeof(ts_data));
      memset(packet, 0, sizeof(packet));
      memset(packets, 0, sizeof(packets));
      memset(&stream, 0, sizeof(stream));
      turbo_rtsp_rtp_stream_init(&stream, 33, 0x01020304, 40, 4000, 90000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_mpeg2_ts(
              &stream,
              ts_data,
              sizeof(ts_data),
              3600,
              TURBO_RTSP_MPEG2_TS_PACKET_SIZE,
              packet,
              sizeof(packet),
              packets,
              1,
              &packet_count),
          -1);
      check_size_eq(packet_count, 0);

      packet_count = 99;
      check_int_eq(
          turbo_rtsp_rtp_stream_write_mpeg2_ts(
              &stream,
              ts_data,
              sizeof(ts_data) - 1,
              3600,
              TURBO_RTSP_MPEG2_TS_PACKET_SIZE,
              packet,
              sizeof(packet),
              packets,
              1,
              &packet_count),
          -1);
      check_int_eq(stream.sequence_number, 40);
      check_int_eq((int)stream.timestamp, 4000);
      check_int_eq((int)stream.sender.packet_count, 0);
      check_int_eq((int)stream.sender.octet_count, 0);
    }

    it("writes H264 single NAL RTP stream packets and advances sender state") {
      const uint8_t nal[] = {0x65, 0x88, 0x99};
      turbo_rtsp_rtp_stream_t stream;
      turbo_rtsp_rtp_stream_packet_t packets[1];
      turbo_rtsp_rtp_header_t parsed;
      uint8_t packet[32];
      size_t packet_count = 0;
      size_t header_len = 0;

      memset(packets, 0, sizeof(packets));
      memset(&parsed, 0, sizeof(parsed));
      turbo_rtsp_rtp_stream_init(&stream, 96, 0x01020304, 100, 90000, 90000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_h264_nal(
              &stream,
              nal,
              sizeof(nal),
              3000,
              1200,
              packet,
              sizeof(packet),
              NULL,
              0,
              packets,
              1,
              &packet_count),
          0);

      check_size_eq(packet_count, 1);
      check(packets[0].packet == packet);
      check_size_eq(packets[0].packet_len, TURBO_RTSP_RTP_HEADER_SIZE + sizeof(nal));
      check_int_eq(
          turbo_rtsp_rtp_parse_header(
              packets[0].packet,
              packets[0].packet_len,
              &parsed,
              &header_len),
          0);
      check_size_eq(header_len, TURBO_RTSP_RTP_HEADER_SIZE);
      check_int_eq(parsed.marker, 1);
      check_int_eq(parsed.payload_type, 96);
      check_int_eq(parsed.sequence_number, 100);
      check_int_eq((int)parsed.timestamp, 90000);
      check_int_eq((int)parsed.ssrc, 0x01020304);
      check_mem_eq(parsed.payload, nal, sizeof(nal));

      check_int_eq(stream.sequence_number, 101);
      check_int_eq((int)stream.timestamp, 93000);
      check_int_eq((int)stream.sender.packet_count, 1);
      check_int_eq((int)stream.sender.octet_count, (int)sizeof(nal));
    }

    it("writes multiple H264 NALs in one access unit with one RTP timestamp") {
      const uint8_t sps[] = {0x67, 0x42, 0x00, 0x1f};
      const uint8_t pps[] = {0x68, 0xce, 0x06, 0xe2};
      const uint8_t slice[] = {0x65, 0x88, 0x84, 0x21};
      const uint8_t *expected_payloads[] = {sps, pps, slice};
      const size_t expected_lens[] = {sizeof(sps), sizeof(pps), sizeof(slice)};
      const int expected_markers[] = {0, 0, 1};
      turbo_rtsp_rtp_stream_t stream;
      turbo_rtsp_rtp_stream_packet_t packets[1];
      uint8_t packets_storage[3][32];
      size_t packet_count = 0;
      size_t i = 0;

      turbo_rtsp_rtp_stream_init(&stream, 96, 0x01020304, 1000, 270000, 90000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_h264_nal_ex(
              &stream,
              sps,
              sizeof(sps),
              0,
              0,
              1200,
              packets_storage[0],
              sizeof(packets_storage[0]),
              NULL,
              0,
              packets,
              1,
              &packet_count),
          0);
      check_size_eq(packet_count, 1);
      check_int_eq((int)stream.timestamp, 270000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_h264_nal_ex(
              &stream,
              pps,
              sizeof(pps),
              0,
              0,
              1200,
              packets_storage[1],
              sizeof(packets_storage[1]),
              NULL,
              0,
              packets,
              1,
              &packet_count),
          0);
      check_size_eq(packet_count, 1);
      check_int_eq((int)stream.timestamp, 270000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_h264_nal_ex(
              &stream,
              slice,
              sizeof(slice),
              1,
              3000,
              1200,
              packets_storage[2],
              sizeof(packets_storage[2]),
              NULL,
              0,
              packets,
              1,
              &packet_count),
          0);
      check_size_eq(packet_count, 1);

      for (i = 0; i < 3; ++i) {
        turbo_rtsp_rtp_header_t parsed;
        size_t header_len = 0;

        memset(&parsed, 0, sizeof(parsed));
        check_int_eq(
            turbo_rtsp_rtp_parse_header(
                packets_storage[i],
                TURBO_RTSP_RTP_HEADER_SIZE + expected_lens[i],
                &parsed,
                &header_len),
            0);
        check_int_eq(parsed.marker, expected_markers[i]);
        check_int_eq(parsed.sequence_number, (uint16_t)(1000 + i));
        check_int_eq((int)parsed.timestamp, 270000);
        check_mem_eq(parsed.payload, expected_payloads[i], expected_lens[i]);
      }

      check_int_eq(stream.sequence_number, 1003);
      check_int_eq((int)stream.timestamp, 273000);
      check_int_eq((int)stream.sender.packet_count, 3);
      check_int_eq(
          (int)stream.sender.octet_count,
          (int)(sizeof(sps) + sizeof(pps) + sizeof(slice)));
    }

    it("writes oversized H264 NALs as FU-A RTP stream packets") {
      const uint8_t nal[] = {0x65, 0x11, 0x22, 0x33, 0x44, 0x55};
      const uint8_t expected_start[] = {0x7c, 0x85, 0x11, 0x22};
      const uint8_t expected_middle[] = {0x7c, 0x05, 0x33, 0x44};
      const uint8_t expected_end[] = {0x7c, 0x45, 0x55};
      const uint8_t *expected_payloads[] = {
          expected_start,
          expected_middle,
          expected_end
      };
      const size_t expected_lens[] = {
          sizeof(expected_start),
          sizeof(expected_middle),
          sizeof(expected_end)
      };
      turbo_rtsp_rtp_stream_t stream;
      turbo_rtsp_rtp_stream_packet_t packets[3];
      uint8_t packet[3 * 16];
      uint8_t payload_scratch[4];
      size_t packet_count = 0;
      size_t i = 0;

      memset(packets, 0, sizeof(packets));
      memset(packet, 0, sizeof(packet));
      memset(payload_scratch, 0, sizeof(payload_scratch));
      turbo_rtsp_rtp_stream_init(&stream, 97, 0x10203040, 65000, 123456, 90000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_h264_nal(
              &stream,
              nal,
              sizeof(nal),
              3600,
              4,
              packet,
              16,
              payload_scratch,
              sizeof(payload_scratch),
              packets,
              3,
              &packet_count),
          0);
      check_size_eq(packet_count, 3);

      for (i = 0; i < packet_count; ++i) {
        turbo_rtsp_rtp_header_t parsed;
        size_t header_len = 0;

        memset(&parsed, 0, sizeof(parsed));
        check(packets[i].packet == packet + (i * 16));
        check_size_eq(packets[i].packet_len, TURBO_RTSP_RTP_HEADER_SIZE + expected_lens[i]);
        check_int_eq(
            turbo_rtsp_rtp_parse_header(
                packets[i].packet,
                packets[i].packet_len,
                &parsed,
                &header_len),
            0);
        check_int_eq(parsed.marker, i == 2 ? 1 : 0);
        check_int_eq(parsed.payload_type, 97);
        check_int_eq(parsed.sequence_number, (uint16_t)(65000 + i));
        check_int_eq((int)parsed.timestamp, 123456);
        check_int_eq((int)parsed.ssrc, 0x10203040);
        check_size_eq(parsed.payload_len, expected_lens[i]);
        check_mem_eq(parsed.payload, expected_payloads[i], expected_lens[i]);
      }

      check_int_eq(stream.sequence_number, 65003);
      check_int_eq((int)stream.timestamp, 127056);
      check_int_eq((int)stream.sender.packet_count, 3);
      check_int_eq((int)stream.sender.octet_count, 11);
    }

    it("writes oversized H265 NALs as FU RTP stream packets") {
      const uint8_t nal[] = {0x26, 0x01, 0x11, 0x22, 0x33, 0x44, 0x55};
      const uint8_t expected_start[] = {0x62, 0x01, 0x93, 0x11, 0x22};
      const uint8_t expected_middle[] = {0x62, 0x01, 0x13, 0x33, 0x44};
      const uint8_t expected_end[] = {0x62, 0x01, 0x53, 0x55};
      const uint8_t *expected_payloads[] = {
          expected_start,
          expected_middle,
          expected_end
      };
      const size_t expected_lens[] = {
          sizeof(expected_start),
          sizeof(expected_middle),
          sizeof(expected_end)
      };
      turbo_rtsp_rtp_stream_t stream;
      turbo_rtsp_rtp_stream_packet_t packets[3];
      uint8_t packet[3 * 20];
      uint8_t payload_scratch[5];
      size_t packet_count = 0;
      size_t i = 0;

      memset(packets, 0, sizeof(packets));
      memset(packet, 0, sizeof(packet));
      memset(payload_scratch, 0, sizeof(payload_scratch));
      turbo_rtsp_rtp_stream_init(&stream, 98, 0x10203040, 65000, 123456, 90000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_h265_nal(
              &stream,
              nal,
              sizeof(nal),
              3600,
              5,
              packet,
              20,
              payload_scratch,
              sizeof(payload_scratch),
              packets,
              3,
              &packet_count),
          0);
      check_size_eq(packet_count, 3);

      for (i = 0; i < packet_count; ++i) {
        turbo_rtsp_rtp_header_t parsed;
        size_t header_len = 0;

        memset(&parsed, 0, sizeof(parsed));
        check(packets[i].packet == packet + (i * 20));
        check_size_eq(packets[i].packet_len, TURBO_RTSP_RTP_HEADER_SIZE + expected_lens[i]);
        check_int_eq(
            turbo_rtsp_rtp_parse_header(
                packets[i].packet,
                packets[i].packet_len,
                &parsed,
                &header_len),
            0);
        check_int_eq(parsed.marker, i == 2 ? 1 : 0);
        check_int_eq(parsed.payload_type, 98);
        check_int_eq(parsed.sequence_number, (uint16_t)(65000 + i));
        check_int_eq((int)parsed.timestamp, 123456);
        check_int_eq((int)parsed.ssrc, 0x10203040);
        check_size_eq(parsed.payload_len, expected_lens[i]);
        check_mem_eq(parsed.payload, expected_payloads[i], expected_lens[i]);
      }

      check_int_eq(stream.sequence_number, 65003);
      check_int_eq((int)stream.timestamp, 127056);
      check_int_eq((int)stream.sender.packet_count, 3);
      check_int_eq((int)stream.sender.octet_count, 14);
    }

    it("does not advance RTP stream state when packet array capacity is too small") {
      const uint8_t nal[] = {0x65, 0x11, 0x22, 0x33, 0x44, 0x55};
      turbo_rtsp_rtp_stream_t stream;
      turbo_rtsp_rtp_stream_packet_t packets[2];
      uint8_t packet[2 * 16];
      uint8_t payload_scratch[4];
      size_t packet_count = 99;

      memset(packets, 0, sizeof(packets));
      turbo_rtsp_rtp_stream_init(&stream, 96, 0x01020304, 10, 1000, 90000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_h264_nal(
              &stream,
              nal,
              sizeof(nal),
              3000,
              4,
              packet,
              16,
              payload_scratch,
              sizeof(payload_scratch),
              packets,
              2,
              &packet_count),
          -1);
      check_size_eq(packet_count, 0);
      check_int_eq(stream.sequence_number, 10);
      check_int_eq((int)stream.timestamp, 1000);
      check_int_eq((int)stream.sender.packet_count, 0);
      check_int_eq((int)stream.sender.octet_count, 0);
    }

    it("rejects invalid H264 RTP stream inputs without advancing state") {
      const uint8_t forbidden_nal[] = {0x85, 0x11};
      const uint8_t oversized_nal[] = {0x65, 0x11, 0x22, 0x33};
      turbo_rtsp_rtp_stream_t stream;
      turbo_rtsp_rtp_stream_packet_t packets[2];
      uint8_t packet[2 * 16];
      uint8_t payload_scratch[4];
      size_t packet_count = 1;

      memset(packets, 0, sizeof(packets));
      turbo_rtsp_rtp_stream_init(&stream, 96, 0x01020304, 20, 2000, 90000);

      check_int_eq(
          turbo_rtsp_rtp_stream_write_h264_nal(
              &stream,
              forbidden_nal,
              sizeof(forbidden_nal),
              3000,
              4,
              packet,
              16,
              payload_scratch,
              sizeof(payload_scratch),
              packets,
              2,
              &packet_count),
          -1);
      check_size_eq(packet_count, 0);
      check_int_eq(
          turbo_rtsp_rtp_stream_write_h264_nal(
              &stream,
              oversized_nal,
              sizeof(oversized_nal),
              3000,
              2,
              packet,
              16,
              payload_scratch,
              sizeof(payload_scratch),
              packets,
              2,
              &packet_count),
          -1);
      check_int_eq(
          turbo_rtsp_rtp_stream_write_h264_nal(
              &stream,
              oversized_nal,
              sizeof(oversized_nal),
              3000,
              4,
              packet,
              15,
              payload_scratch,
              sizeof(payload_scratch),
              packets,
              2,
              &packet_count),
          -1);
      check_int_eq(stream.sequence_number, 20);
      check_int_eq((int)stream.timestamp, 2000);
      check_int_eq((int)stream.sender.packet_count, 0);
      check_int_eq((int)stream.sender.octet_count, 0);
    }

    it("receives H264 single NAL RTP packets directly from packet payload") {
      const uint8_t payload[] = {0x65, 0x88, 0x99};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          100,
          90000,
          0x01020304,
          payload,
          sizeof(payload));
      check(packet_len > 0);

      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check(nal == packet + TURBO_RTSP_RTP_HEADER_SIZE);
      check_size_eq(nal_len, sizeof(payload));
      check_mem_eq(nal, payload, sizeof(payload));
    }

    it("receives H264 FU-A RTP packets as partial then complete NAL") {
      const uint8_t expected_nal[] = {0x65, 0x88, 0x99, 0xaa};
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88, 0x99};
      const uint8_t end_payload[] = {0x7c, 0x45, 0xaa};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      uint8_t nal_buffer[sizeof(expected_nal)];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          200,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
      check_int_eq(stream.reassembler.started, 1);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          201,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90010,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check(nal == nal_buffer);
      check_size_eq(nal_len, sizeof(expected_nal));
      check_mem_eq(nal, expected_nal, sizeof(expected_nal));
      check_int_eq(stream.reassembler.started, 0);
    }

    it("recovers short out-of-order H264 FU-A RTP packets") {
      const uint8_t expected_nal[] = {0x65, 0x88, 0x99, 0xaa};
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88};
      const uint8_t middle_payload[] = {0x7c, 0x05, 0x99};
      const uint8_t end_payload[] = {0x7c, 0x45, 0xaa};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      uint8_t nal_buffer[sizeof(expected_nal)];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          10,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          12,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90020,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check_size_eq(stream.reorder_count, 1);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          11,
          90000,
          0x01020304,
          middle_payload,
          sizeof(middle_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90010,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_LATE_OR_OUT_OF_ORDER);
      check(nal == nal_buffer);
      check_size_eq(nal_len, sizeof(expected_nal));
      check_mem_eq(nal, expected_nal, sizeof(expected_nal));
      check_size_eq(stream.reorder_count, 0);
      check_int_eq(stream.reassembler.started, 0);
    }

    it("drains queued out-of-order H264 single NAL RTP packets") {
      const uint8_t payload_10[] = {0x41, 0x10};
      const uint8_t payload_11[] = {0x41, 0x11};
      const uint8_t payload_12[] = {0x41, 0x12};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          10,
          90000,
          0x01020304,
          payload_10,
          sizeof(payload_10));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check_mem_eq(nal, payload_10, sizeof(payload_10));

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          12,
          90000,
          0x01020304,
          payload_12,
          sizeof(payload_12));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90020,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check_size_eq(stream.reorder_count, 1);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          11,
          90000,
          0x01020304,
          payload_11,
          sizeof(payload_11));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90010,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_LATE_OR_OUT_OF_ORDER);
      check_mem_eq(nal, payload_11, sizeof(payload_11));
      check_size_eq(stream.reorder_count, 1);

      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_drain_queued(
              &stream,
              NULL,
              0,
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_OK);
      check_size_eq(nal_len, sizeof(payload_12));
      check_mem_eq(nal, payload_12, sizeof(payload_12));
      check_size_eq(stream.reorder_count, 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_drain_queued(
              &stream,
              NULL,
              0,
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_PARTIAL);
      check_null(nal);
      check_size_eq(nal_len, 0);
    }

    it("does not re-output duplicate H264 RTP packets") {
      const uint8_t payload[] = {0x41, 0x9a, 0xbc};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          300,
          90000,
          0x01020304,
          payload,
          sizeof(payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check(nal != NULL);

      nal = (const uint8_t *)0x1;
      nal_len = 99;
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_DUPLICATE);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
    }

    it("resets H264 receive reassembly on RTP dropout") {
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88};
      const uint8_t end_payload[] = {0x7c, 0x45, 0xaa};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      uint8_t nal_buffer[8];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          10,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(stream.reassembler.started, 1);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          4011,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90010,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_ERROR);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_DROPOUT);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
      check_int_eq(stream.reassembler.started, 0);
    }

    it("resets H264 receive reassembly on RTP sequence gaps beyond the reorder window") {
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88};
      const uint8_t end_payload[] = {0x7c, 0x45, 0xaa};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      uint8_t nal_buffer[8];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          40,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          50,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90010,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_ERROR);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
      check_int_eq(stream.reassembler.started, 0);
      check_size_eq(stream.reorder_count, 0);
    }

    it("resets H264 receive reassembly when the reorder queue is full") {
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88};
      const uint8_t middle_payload[] = {0x7c, 0x05, 0x99};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      uint8_t nal_buffer[16];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;
      uint16_t sequence = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          100,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);

      for (sequence = 102; sequence <= 105; ++sequence) {
        packet_len = turbo_rtsp_test_write_rtp_packet(
            packet,
            sizeof(packet),
            sequence,
            90000,
            0x01020304,
            middle_payload,
            sizeof(middle_payload));
        check(packet_len > 0);
        check_int_eq(
            turbo_rtsp_rtp_h264_receive_stream_push(
                &stream,
                packet,
                packet_len,
                90000 + (uint32_t)sequence,
                nal_buffer,
                sizeof(nal_buffer),
                &nal,
                &nal_len,
                &update_result),
            TURBO_RTSP_FRAME_PARTIAL);
      }
      check_size_eq(stream.reorder_count, TURBO_RTSP_RTP_H264_RECEIVE_REORDER_CAPACITY);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          106,
          90000,
          0x01020304,
          middle_payload,
          sizeof(middle_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90106,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_ERROR);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check_size_eq(stream.reorder_count, 0);
      check_int_eq(stream.reassembler.started, 0);
    }

    it("clears queued H264 RTP packets when receive stream state resets") {
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88};
      const uint8_t end_payload[] = {0x7c, 0x45, 0xaa};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      uint8_t nal_buffer[8];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          200,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          202,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90020,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_size_eq(stream.reorder_count, 1);

      turbo_rtsp_rtp_h264_receive_stream_reset(&stream, 0x10203040);
      check_size_eq(stream.reorder_count, 0);
      check_int_eq(stream.has_next_sequence_number, 0);
      check_int_eq(stream.reassembler.started, 0);
    }

    it("resets H264 receive reassembly on SSRC mismatch") {
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88};
      const uint8_t end_payload[] = {0x7c, 0x45, 0xaa};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      uint8_t nal_buffer[8];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0x01020304);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          20,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(stream.reassembler.started, 1);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          21,
          90000,
          0x10203040,
          end_payload,
          sizeof(end_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90010,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_ERROR);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_SSRC_MISMATCH);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
      check_int_eq(stream.reassembler.started, 0);
    }

    it("returns error and clears H264 receive reassembly when NAL buffer is too small") {
      const uint8_t start_payload[] = {0x7c, 0x85, 0x88, 0x99};
      turbo_rtsp_rtp_h264_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      uint8_t nal_buffer[2];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h264_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          30,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h264_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_ERROR);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
      check_int_eq(stream.reassembler.started, 0);
    }

    it("receives H265 single NAL RTP packets directly from packet payload") {
      const uint8_t payload[] = {0x26, 0x01, 0x88, 0x99};
      turbo_rtsp_rtp_h265_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h265_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          100,
          90000,
          0x01020304,
          payload,
          sizeof(payload));
      check(packet_len > 0);

      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check(nal == packet + TURBO_RTSP_RTP_HEADER_SIZE);
      check_size_eq(nal_len, sizeof(payload));
      check_mem_eq(nal, payload, sizeof(payload));
    }

    it("receives H265 FU RTP packets as partial then complete NAL") {
      const uint8_t expected_nal[] = {0x26, 0x01, 0x88, 0x99, 0xaa};
      const uint8_t start_payload[] = {0x62, 0x01, 0x93, 0x88, 0x99};
      const uint8_t end_payload[] = {0x62, 0x01, 0x53, 0xaa};
      turbo_rtsp_rtp_h265_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      uint8_t nal_buffer[sizeof(expected_nal)];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h265_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          200,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
      check_int_eq(stream.reassembler.started, 1);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          201,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90010,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check(nal == nal_buffer);
      check_size_eq(nal_len, sizeof(expected_nal));
      check_mem_eq(nal, expected_nal, sizeof(expected_nal));
      check_int_eq(stream.reassembler.started, 0);
    }

    it("recovers short out-of-order H265 FU RTP packets") {
      const uint8_t expected_nal[] = {0x26, 0x01, 0x88, 0x99, 0xaa};
      const uint8_t start_payload[] = {0x62, 0x01, 0x93, 0x88};
      const uint8_t middle_payload[] = {0x62, 0x01, 0x13, 0x99};
      const uint8_t end_payload[] = {0x62, 0x01, 0x53, 0xaa};
      turbo_rtsp_rtp_h265_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      uint8_t nal_buffer[sizeof(expected_nal)];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h265_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          10,
          90000,
          0x01020304,
          start_payload,
          sizeof(start_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          12,
          90000,
          0x01020304,
          end_payload,
          sizeof(end_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90020,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check_size_eq(stream.reorder_count, 1);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          11,
          90000,
          0x01020304,
          middle_payload,
          sizeof(middle_payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90010,
              nal_buffer,
              sizeof(nal_buffer),
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_LATE_OR_OUT_OF_ORDER);
      check(nal == nal_buffer);
      check_size_eq(nal_len, sizeof(expected_nal));
      check_mem_eq(nal, expected_nal, sizeof(expected_nal));
      check_size_eq(stream.reorder_count, 0);
      check_int_eq(stream.reassembler.started, 0);
    }

    it("drains queued out-of-order H265 single NAL RTP packets") {
      const uint8_t payload_10[] = {0x02, 0x01, 0x10};
      const uint8_t payload_11[] = {0x02, 0x01, 0x11};
      const uint8_t payload_12[] = {0x02, 0x01, 0x12};
      turbo_rtsp_rtp_h265_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h265_receive_stream_init(&stream, 0);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          10,
          90000,
          0x01020304,
          payload_10,
          sizeof(payload_10));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check_mem_eq(nal, payload_10, sizeof(payload_10));

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          12,
          90000,
          0x01020304,
          payload_12,
          sizeof(payload_12));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90020,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_OK);
      check_size_eq(stream.reorder_count, 1);

      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          11,
          90000,
          0x01020304,
          payload_11,
          sizeof(payload_11));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90010,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_LATE_OR_OUT_OF_ORDER);
      check_mem_eq(nal, payload_11, sizeof(payload_11));
      check_size_eq(stream.reorder_count, 1);

      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_drain_queued(
              &stream,
              NULL,
              0,
              &nal,
              &nal_len),
          TURBO_RTSP_FRAME_OK);
      check_size_eq(nal_len, sizeof(payload_12));
      check_mem_eq(nal, payload_12, sizeof(payload_12));
      check_size_eq(stream.reorder_count, 0);
    }

    it("does not re-output duplicate H265 RTP packets") {
      const uint8_t payload[] = {0x26, 0x01, 0x88, 0x99};
      turbo_rtsp_rtp_h265_receive_stream_t stream;
      turbo_rtsp_rtp_source_update_result_t update_result;
      uint8_t packet[32];
      const uint8_t *nal = NULL;
      size_t nal_len = 0;
      size_t packet_len = 0;

      turbo_rtsp_rtp_h265_receive_stream_init(&stream, 0);
      packet_len = turbo_rtsp_test_write_rtp_packet(
          packet,
          sizeof(packet),
          300,
          90000,
          0x01020304,
          payload,
          sizeof(payload));
      check(packet_len > 0);
      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_OK);
      check(nal != NULL);

      nal = (const uint8_t *)0x1;
      nal_len = 99;
      check_int_eq(
          turbo_rtsp_rtp_h265_receive_stream_push(
              &stream,
              packet,
              packet_len,
              90000,
              NULL,
              0,
              &nal,
              &nal_len,
              &update_result),
          TURBO_RTSP_FRAME_PARTIAL);
      check_int_eq(update_result, TURBO_RTSP_RTP_SOURCE_UPDATE_DUPLICATE);
      check(nal == NULL);
      check_size_eq(nal_len, 0);
    }

    it("binds RTP sources to an expected SSRC when configured") {
      turbo_rtsp_rtp_source_t source;
      turbo_rtsp_rtp_header_t header;

      memset(&header, 0, sizeof(header));
      header.version = 2;
      header.sequence_number = 1;
      header.timestamp = 1000;
      header.ssrc = 0x01020304;

      turbo_rtsp_rtp_source_init(&source, 0x10203040);
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 1000), -1);

      turbo_rtsp_rtp_source_init(&source, 0x01020304);
      check_int_eq(turbo_rtsp_rtp_source_update(&source, &header, 1000), 0);
      check_int_eq((int)source.ssrc, 0x01020304);
      check_int_eq(source.has_ssrc, 1);
      check_int_eq(source.initialized, 1);
    }
  }

  section("RFC 2326 RTP over RTSP interleaving") {
    it("parses a complete interleaved binary frame") {
      const uint8_t frame_bytes[] = {'$', 2, 0, 4, 0xde, 0xad, 0xbe, 0xef, 'R', 'T'};
      turbo_rtsp_interleaved_frame_t frame;
      size_t consumed = 0;
      int rc = turbo_rtsp_interleaved_parse(
          frame_bytes,
          sizeof(frame_bytes),
          &frame,
          &consumed);

      check_int_eq(rc, TURBO_RTSP_FRAME_OK);
      check_int_eq(frame.channel, 2);
      check_int_eq(frame.payload_len, 4);
      check_mem_eq(frame.payload, frame_bytes + TURBO_RTSP_INTERLEAVED_HEADER_SIZE, 4);
      check_size_eq(consumed, TURBO_RTSP_INTERLEAVED_HEADER_SIZE + 4);
    }

    it("reports partial interleaved frames until the declared payload arrives") {
      const uint8_t frame_bytes[] = {'$', 0, 0, 4, 0xde, 0xad};
      turbo_rtsp_interleaved_frame_t frame;
      size_t consumed = 0;

      check_int_eq(
          turbo_rtsp_interleaved_parse(frame_bytes, sizeof(frame_bytes), &frame, &consumed),
          TURBO_RTSP_FRAME_PARTIAL);
      check_size_eq(consumed, 0);
    }

    it("skips noise before parsing the next interleaved frame") {
      const uint8_t frame_bytes[] = {'R', 'T', '$', 3, 0, 2, 0xca, 0xfe};
      turbo_rtsp_interleaved_parser_t parser;
      turbo_rtsp_interleaved_frame_t frame;
      size_t consumed = 0;

      turbo_rtsp_interleaved_parser_init(&parser);
      check_int_eq(
          turbo_rtsp_interleaved_parser_parse(
              &parser,
              frame_bytes,
              sizeof(frame_bytes),
              &frame,
              &consumed),
          TURBO_RTSP_FRAME_OK);
      check_size_eq(consumed, sizeof(frame_bytes));
      check_int_eq(frame.channel, 3);
      check_int_eq(frame.payload_len, 2);
      check_mem_eq(frame.payload, frame_bytes + 6, 2);
    }

    it("keeps interleaved header state across input buffers") {
      const uint8_t header_part[] = {'$', 4};
      const uint8_t payload_part[] = {0, 3, 0x11, 0x22, 0x33};
      turbo_rtsp_interleaved_parser_t parser;
      turbo_rtsp_interleaved_frame_t frame;
      size_t consumed = 0;

      turbo_rtsp_interleaved_parser_init(&parser);
      check_int_eq(
          turbo_rtsp_interleaved_parser_parse(
              &parser,
              header_part,
              sizeof(header_part),
              &frame,
              &consumed),
          TURBO_RTSP_FRAME_PARTIAL);
      check_size_eq(consumed, sizeof(header_part));
      check_size_eq(parser.needed, 2);

      check_int_eq(
          turbo_rtsp_interleaved_parser_parse(
              &parser,
              payload_part,
              sizeof(payload_part),
              &frame,
              &consumed),
          TURBO_RTSP_FRAME_OK);
      check_size_eq(consumed, sizeof(payload_part));
      check_int_eq(frame.channel, 4);
      check_int_eq(frame.payload_len, 3);
      check_mem_eq(frame.payload, payload_part + 2, 3);
    }

    it("records missing payload bytes when zero-copy output would cross buffers") {
      const uint8_t first_part[] = {'$', 0, 0, 4, 0xde, 0xad};
      const uint8_t second_part[] = {0xbe};
      const uint8_t third_part[] = {0xef, '$', 1, 0, 1, 0x7a};
      turbo_rtsp_interleaved_parser_t parser;
      turbo_rtsp_interleaved_frame_t frame;
      size_t consumed = 0;

      turbo_rtsp_interleaved_parser_init(&parser);
      check_int_eq(
          turbo_rtsp_interleaved_parser_parse(
              &parser,
              first_part,
              sizeof(first_part),
              &frame,
              &consumed),
          TURBO_RTSP_FRAME_PARTIAL);
      check_size_eq(consumed, sizeof(first_part));
      check_size_eq(parser.needed, 2);
      check_int_eq(parser.discarding_payload, 1);

      check_int_eq(
          turbo_rtsp_interleaved_parser_parse(
              &parser,
              second_part,
              sizeof(second_part),
              &frame,
              &consumed),
          TURBO_RTSP_FRAME_PARTIAL);
      check_size_eq(consumed, sizeof(second_part));
      check_size_eq(parser.needed, 1);

      check_int_eq(
          turbo_rtsp_interleaved_parser_parse(
              &parser,
              third_part,
              sizeof(third_part),
              &frame,
              &consumed),
          TURBO_RTSP_FRAME_PARTIAL);
      check_size_eq(consumed, 1);
      check_size_eq(parser.needed, 0);

      check_int_eq(
          turbo_rtsp_interleaved_parser_parse(
              &parser,
              third_part + consumed,
              sizeof(third_part) - consumed,
              &frame,
              &consumed),
          TURBO_RTSP_FRAME_OK);
      check_size_eq(consumed, sizeof(third_part) - 1);
      check_int_eq(frame.channel, 1);
      check_int_eq(frame.payload_len, 1);
      check_mem_eq(frame.payload, third_part + 5, 1);
    }

    it("copies an interleaved frame split across three input buffers") {
      const uint8_t first_part[] = {'$', 8};
      const uint8_t second_part[] = {0, 4, 0xde};
      const uint8_t third_part[] = {0xad, 0xbe, 0xef, '$', 1, 0, 1, 0x7a};
      const uint8_t expected_payload[] = {0xde, 0xad, 0xbe, 0xef};
      turbo_rtsp_interleaved_parser_t parser;
      turbo_rtsp_interleaved_frame_t frame;
      uint8_t payload[sizeof(expected_payload)];
      size_t consumed = 0;

      turbo_rtsp_interleaved_parser_init(&parser);
      check_int_eq(
          turbo_rtsp_interleaved_parser_parse_copy(
              &parser,
              first_part,
              sizeof(first_part),
              &frame,
              payload,
              sizeof(payload),
              &consumed),
          TURBO_RTSP_FRAME_PARTIAL);
      check_size_eq(consumed, sizeof(first_part));

      check_int_eq(
          turbo_rtsp_interleaved_parser_parse_copy(
              &parser,
              second_part,
              sizeof(second_part),
              &frame,
              payload,
              sizeof(payload),
              &consumed),
          TURBO_RTSP_FRAME_PARTIAL);
      check_size_eq(consumed, sizeof(second_part));

      check_int_eq(
          turbo_rtsp_interleaved_parser_parse_copy(
              &parser,
              third_part,
              sizeof(third_part),
              &frame,
              payload,
              sizeof(payload),
              &consumed),
          TURBO_RTSP_FRAME_OK);
      check_size_eq(consumed, 3);
      check_int_eq(frame.channel, 8);
      check_int_eq(frame.payload_len, 4);
      check(frame.payload == payload);
      check_mem_eq(frame.payload, expected_payload, sizeof(expected_payload));

      turbo_rtsp_interleaved_parser_destroy(&parser);
    }

    it("keeps copied interleaved frames pending for small output buffer retry") {
      const uint8_t frame_bytes[] = {'$', 9, 0, 4, 0xde, 0xad, 0xbe, 0xef};
      const uint8_t expected_payload[] = {0xde, 0xad, 0xbe, 0xef};
      turbo_rtsp_interleaved_parser_t parser;
      turbo_rtsp_interleaved_frame_t frame;
      uint8_t short_payload[2];
      uint8_t payload[sizeof(expected_payload)];
      size_t consumed = 0;

      turbo_rtsp_interleaved_parser_init(&parser);
      check_int_eq(
          turbo_rtsp_interleaved_parser_parse_copy(
              &parser,
              frame_bytes,
              sizeof(frame_bytes),
              &frame,
              short_payload,
              sizeof(short_payload),
              &consumed),
          TURBO_RTSP_FRAME_ERROR);
      check_size_eq(consumed, sizeof(frame_bytes));
      check_int_eq(frame.channel, 9);
      check_int_eq(frame.payload_len, 4);
      check_size_eq(parser.needed, sizeof(expected_payload));
      check(frame.payload == NULL);

      check_int_eq(
          turbo_rtsp_interleaved_parser_parse_copy(
              &parser,
              NULL,
              0,
              &frame,
              payload,
              sizeof(payload),
              &consumed),
          TURBO_RTSP_FRAME_OK);
      check_size_eq(consumed, 0);
      check_int_eq(frame.channel, 9);
      check_int_eq(frame.payload_len, 4);
      check(frame.payload == payload);
      check_mem_eq(frame.payload, expected_payload, sizeof(expected_payload));

      turbo_rtsp_interleaved_parser_destroy(&parser);
    }

    it("parses consecutive interleaved frames with one parser state") {
      const uint8_t frame_bytes[] = {
          '$', 1, 0, 1, 0xaa,
          '$', 2, 0, 2, 0xbb, 0xcc
      };
      turbo_rtsp_interleaved_parser_t parser;
      turbo_rtsp_interleaved_frame_t frame;
      size_t consumed = 0;
      size_t total_consumed = 0;

      turbo_rtsp_interleaved_parser_init(&parser);
      check_int_eq(
          turbo_rtsp_interleaved_parser_parse(
              &parser,
              frame_bytes,
              sizeof(frame_bytes),
              &frame,
              &consumed),
          TURBO_RTSP_FRAME_OK);
      check_size_eq(consumed, 5);
      check_int_eq(frame.channel, 1);
      check_mem_eq(frame.payload, frame_bytes + 4, 1);
      total_consumed += consumed;

      check_int_eq(
          turbo_rtsp_interleaved_parser_parse(
              &parser,
              frame_bytes + total_consumed,
              sizeof(frame_bytes) - total_consumed,
              &frame,
              &consumed),
          TURBO_RTSP_FRAME_OK);
      check_size_eq(consumed, 6);
      check_int_eq(frame.channel, 2);
      check_mem_eq(frame.payload, frame_bytes + 9, 2);
    }

    it("formats an interleaved frame header") {
      uint8_t header[TURBO_RTSP_INTERLEAVED_HEADER_SIZE];

      check_int_eq(turbo_rtsp_interleaved_write_header(header, sizeof(header), 1, 1500), 4);
      check_int_eq(header[0], '$');
      check_int_eq(header[1], 1);
      check_int_eq(header[2], 0x05);
      check_int_eq(header[3], 0xdc);
    }

    it("rejects invalid interleaved frame header output buffers") {
      uint8_t short_header[TURBO_RTSP_INTERLEAVED_HEADER_SIZE - 1] = {0xaa, 0xbb, 0xcc};

      check_int_eq(turbo_rtsp_interleaved_write_header(NULL, 0, 1, 1), -1);
      check_int_eq(
          turbo_rtsp_interleaved_write_header(
              short_header,
              sizeof(short_header),
              1,
              1),
          -1);
      check_int_eq(short_header[0], 0xaa);
      check_int_eq(short_header[1], 0xbb);
      check_int_eq(short_header[2], 0xcc);
    }

    it("formats the largest interleaved frame header values") {
      uint8_t header[TURBO_RTSP_INTERLEAVED_HEADER_SIZE];

      check_int_eq(turbo_rtsp_interleaved_write_header(header, sizeof(header), 255, 65535), 4);
      check_int_eq(header[0], '$');
      check_int_eq(header[1], 255);
      check_int_eq(header[2], 0xff);
      check_int_eq(header[3], 0xff);
    }
  }

  section("RFC 4566 SDP responses") {
    it("builds a single-media SDP description") {
      const turbo_rtsp_sdp_session_t session = {
          "-",
          2890844526u,
          2890842807u,
          "192.0.2.1",
          "RTSP Session",
          "192.0.2.1",
          "npt=0-60.000",
          "recvonly"
      };
      const turbo_rtsp_sdp_media_t media = {
          "video",
          0,
          "RTP/AVP",
          96,
          "H264",
          90000,
          "trackID=0",
          "packetization-mode=1;profile-level-id=42e01f",
          "npt=10-20",
          "sendonly",
          "198.51.100.10"
      };
      const char *expected =
          "v=0\r\n"
          "o=- 2890844526 2890842807 IN IP4 192.0.2.1\r\n"
          "s=RTSP Session\r\n"
          "c=IN IP4 192.0.2.1\r\n"
          "t=0 0\r\n"
          "a=range:npt=0-60.000\r\n"
          "a=recvonly\r\n"
          "m=video 0 RTP/AVP 96\r\n"
          "c=IN IP4 198.51.100.10\r\n"
          "a=rtpmap:96 H264/90000\r\n"
          "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
          "a=control:trackID=0\r\n"
          "a=range:npt=10-20\r\n"
          "a=sendonly\r\n";
      char buffer[512];
      int len = turbo_rtsp_sdp_build(buffer, sizeof(buffer), &session, &media, 1);

      check_int_eq(len, (int)strlen(expected));
      check_str_eq(buffer, expected);
    }

    it("parses media rtpmap and control attributes") {
      static const char sdp_body[] =
          "v=0\r\n"
          "o=- 2890844526 2890842807 IN IP4 192.0.2.1\r\n"
          "s=RTSP Session\r\n"
          "c=IN IP4 192.0.2.1\r\n"
          "t=0 0\r\n"
          "a=control:*\r\n"
          "a=range:npt=0-60.000\r\n"
          "a=recvonly\r\n"
          "m=video 0 RTP/AVP 96\r\n"
          "c=IN IP4 198.51.100.10\r\n"
          "a=rtpmap:96 H264/90000\r\n"
          "a=fmtp:97 should=be-ignored\r\n"
          "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f;sprop-parameter-sets=Z0IAH5WoFAFuQA==,aM48gA==\r\n"
          "a=control:trackID=0\r\n"
          "a=range:npt=10-20\r\n"
          "a=sendonly\r\n"
          "m=audio 0 RTP/AVP 97\r\n"
          "a=rtpmap:97 MPEG4-GENERIC/48000/2\r\n"
          "a=fmtp:97 streamtype=5;mode=AAC-hbr;config=1190\r\n"
          "a=control:trackID=1\r\n";
      const uint8_t expected_h264_annexb[] = {
          0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1f, 0x95, 0xa8, 0x14, 0x01, 0x6e, 0x40,
          0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80
      };
      const uint8_t expected_config[] = {0x11, 0x90};
      static turbo_rtsp_sdp_description_t parsed;
      uint8_t h264_annexb[sizeof(expected_h264_annexb)];
      uint8_t config[sizeof(expected_config)];
      size_t h264_written = 0;
      size_t written = 0;

      memset(&parsed, 0, sizeof(parsed));
      memset(h264_annexb, 0, sizeof(h264_annexb));
      memset(config, 0, sizeof(config));

      check_int_eq(turbo_rtsp_sdp_parse(sdp_body, sizeof(sdp_body) - 1, &parsed), 0);
      check_str_eq(parsed.session_name, "RTSP Session");
      check_str_eq(parsed.connection_address, "192.0.2.1");
      check_str_eq(parsed.control, "*");
      check_str_eq(parsed.range, "npt=0-60.000");
      check_str_eq(parsed.direction, "recvonly");
      check_size_eq(parsed.media_count, 2);
      check_str_eq(parsed.media[0].media, "video");
      check_str_eq(parsed.media[0].connection_address, "198.51.100.10");
      check_str_eq(parsed.media[0].proto, "RTP/AVP");
      check_int_eq(parsed.media[0].payload_type, 96);
      check_str_eq(parsed.media[0].encoding_name, "H264");
      check_int_eq(parsed.media[0].clock_rate, 90000);
      check_int_eq(parsed.media[0].encoding_parameters, 0);
      check_int_eq((int)parsed.media[0].mpeg4_fmtp.flags, 0);
      check_int_eq(
          (int)parsed.media[0].h264_fmtp.flags,
          (int)(TURBO_RTSP_SDP_H264_FMTP_PACKETIZATION_MODE |
                TURBO_RTSP_SDP_H264_FMTP_PROFILE_LEVEL_ID |
                TURBO_RTSP_SDP_H264_FMTP_SPROP_PARAMETER_SETS));
      check_int_eq(parsed.media[0].h264_fmtp.packetization_mode, 1);
      check_str_eq(parsed.media[0].h264_fmtp.profile_level_id, "42e01f");
      check_str_eq(
          parsed.media[0].h264_fmtp.sprop_parameter_sets,
          "Z0IAH5WoFAFuQA==,aM48gA==");
      check_int_eq(
          turbo_rtsp_sdp_h264_fmtp_write_annexb(
              h264_annexb,
              sizeof(h264_annexb),
              &parsed.media[0].h264_fmtp,
              &h264_written),
          0);
      check_size_eq(h264_written, sizeof(expected_h264_annexb));
      check_mem_eq(h264_annexb, expected_h264_annexb, sizeof(expected_h264_annexb));
      check_str_eq(
          parsed.media[0].fmtp,
          "packetization-mode=1;profile-level-id=42e01f;sprop-parameter-sets=Z0IAH5WoFAFuQA==,aM48gA==");
      check_str_eq(parsed.media[0].control, "trackID=0");
      check_str_eq(parsed.media[0].range, "npt=10-20");
      check_str_eq(parsed.media[0].direction, "sendonly");
      check_str_eq(parsed.media[1].media, "audio");
      check_str_eq(parsed.media[1].connection_address, "");
      check_int_eq(parsed.media[1].payload_type, 97);
      check_str_eq(parsed.media[1].encoding_name, "MPEG4-GENERIC");
      check_int_eq(parsed.media[1].clock_rate, 48000);
      check_int_eq(parsed.media[1].encoding_parameters, 2);
      check_str_eq(parsed.media[1].fmtp, "streamtype=5;mode=AAC-hbr;config=1190");
      check_int_eq(
          (int)parsed.media[1].mpeg4_fmtp.flags,
          (int)(TURBO_RTSP_SDP_MPEG4_FMTP_STREAM_TYPE |
                TURBO_RTSP_SDP_MPEG4_FMTP_MODE |
                TURBO_RTSP_SDP_MPEG4_FMTP_CONFIG));
      check_int_eq(parsed.media[1].mpeg4_fmtp.stream_type, 5);
      check_str_eq(parsed.media[1].mpeg4_fmtp.mode, "AAC-hbr");
      check_str_eq(parsed.media[1].mpeg4_fmtp.config, "1190");
      check_int_eq(
          turbo_rtsp_sdp_mpeg4_fmtp_write_config(
              config,
              sizeof(config),
              &parsed.media[1].mpeg4_fmtp,
              &written),
          0);
      check_size_eq(written, sizeof(expected_config));
      check_mem_eq(config, expected_config, sizeof(expected_config));
      check_str_eq(parsed.media[1].control, "trackID=1");
      check_str_eq(parsed.media[1].range, "");
      check_str_eq(parsed.media[1].direction, "");
    }

    it("parses MP4A-LATM fmtp fields and writes AAC config") {
      static const char sdp_body[] =
          "v=0\r\n"
          "s=RTSP Session\r\n"
          "m=audio 0 RTP/AVP 96\r\n"
          "a=rtpmap:96 MP4A-LATM/48000/2\r\n"
          "a=fmtp:96 profile-level-id=9;object=8;cpresent=0;config=9128B1071070\r\n";
      const uint8_t expected_config[] = {0x91, 0x28, 0xb1, 0x07, 0x10, 0x70};
      static turbo_rtsp_sdp_description_t parsed;
      uint8_t config[sizeof(expected_config)];
      size_t written = 0;

      memset(&parsed, 0, sizeof(parsed));
      memset(config, 0, sizeof(config));

      check_int_eq(turbo_rtsp_sdp_parse(sdp_body, sizeof(sdp_body) - 1, &parsed), 0);
      check_size_eq(parsed.media_count, 1);
      check_str_eq(parsed.media[0].encoding_name, "MP4A-LATM");
      check_int_eq(parsed.media[0].clock_rate, 48000);
      check_int_eq(parsed.media[0].encoding_parameters, 2);
      check_int_eq(
          (int)parsed.media[0].mpeg4_fmtp.flags,
          (int)(TURBO_RTSP_SDP_MPEG4_FMTP_PROFILE_LEVEL_ID |
                TURBO_RTSP_SDP_MPEG4_FMTP_OBJECT |
                TURBO_RTSP_SDP_MPEG4_FMTP_CPRESENT |
                TURBO_RTSP_SDP_MPEG4_FMTP_CONFIG));
      check_str_eq(parsed.media[0].mpeg4_fmtp.profile_level_id, "9");
      check_int_eq(parsed.media[0].mpeg4_fmtp.object, 8);
      check_int_eq(parsed.media[0].mpeg4_fmtp.cpresent, 0);
      check_str_eq(parsed.media[0].mpeg4_fmtp.config, "9128B1071070");
      check_int_eq(
          turbo_rtsp_sdp_mpeg4_fmtp_write_config(
              config,
              sizeof(config),
              &parsed.media[0].mpeg4_fmtp,
              &written),
          0);
      check_size_eq(written, sizeof(expected_config));
      check_mem_eq(config, expected_config, sizeof(expected_config));
    }

    it("applies RTP/AVP static payload profiles when rtpmap is omitted") {
      static const char sdp_body[] =
          "v=0\r\n"
          "s=RTSP Session\r\n"
          "m=audio 0 RTP/AVP 0\r\n"
          "a=control:trackID=0\r\n"
          "m=audio 0 RTP/AVP/TCP 8\r\n"
          "a=control:trackID=1\r\n"
          "m=video 0 RTP/AVP 33\r\n"
          "a=control:trackID=2\r\n";
      static turbo_rtsp_sdp_description_t parsed;

      memset(&parsed, 0, sizeof(parsed));
      check_int_eq(turbo_rtsp_sdp_parse(sdp_body, sizeof(sdp_body) - 1, &parsed), 0);
      check_size_eq(parsed.media_count, 3);
      check_int_eq(parsed.media[0].payload_type, 0);
      check_str_eq(parsed.media[0].encoding_name, "PCMU");
      check_int_eq(parsed.media[0].clock_rate, 8000);
      check_int_eq(parsed.media[0].encoding_parameters, 1);
      check_str_eq(parsed.media[0].control, "trackID=0");
      check_int_eq(parsed.media[1].payload_type, 8);
      check_str_eq(parsed.media[1].encoding_name, "PCMA");
      check_int_eq(parsed.media[1].clock_rate, 8000);
      check_int_eq(parsed.media[1].encoding_parameters, 1);
      check_str_eq(parsed.media[1].control, "trackID=1");
      check_int_eq(parsed.media[2].payload_type, 33);
      check_str_eq(parsed.media[2].encoding_name, "MP2T");
      check_int_eq(parsed.media[2].clock_rate, 90000);
      check_int_eq(parsed.media[2].encoding_parameters, 0);
      check_str_eq(parsed.media[2].control, "trackID=2");
    }

    it("builds MPEG4 AAC fmtp strings") {
      turbo_rtsp_sdp_mpeg4_fmtp_t mpeg4;
      char fmtp[160];

      memset(&mpeg4, 0, sizeof(mpeg4));
      mpeg4.flags = TURBO_RTSP_SDP_MPEG4_FMTP_STREAM_TYPE |
                    TURBO_RTSP_SDP_MPEG4_FMTP_PROFILE_LEVEL_ID |
                    TURBO_RTSP_SDP_MPEG4_FMTP_MODE |
                    TURBO_RTSP_SDP_MPEG4_FMTP_SIZE_LENGTH |
                    TURBO_RTSP_SDP_MPEG4_FMTP_INDEX_LENGTH |
                    TURBO_RTSP_SDP_MPEG4_FMTP_INDEX_DELTA_LENGTH |
                    TURBO_RTSP_SDP_MPEG4_FMTP_CONFIG;
      mpeg4.stream_type = 5;
      snprintf(mpeg4.profile_level_id, sizeof(mpeg4.profile_level_id), "41");
      snprintf(mpeg4.mode, sizeof(mpeg4.mode), "AAC-hbr");
      mpeg4.size_length = 13;
      mpeg4.index_length = 3;
      mpeg4.index_delta_length = 3;
      snprintf(mpeg4.config, sizeof(mpeg4.config), "1190");

      check_int_eq(
          turbo_rtsp_sdp_mpeg4_fmtp_build(fmtp, sizeof(fmtp), &mpeg4),
          (int)strlen("streamtype=5;profile-level-id=41;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1190"));
      check_str_eq(
          fmtp,
          "streamtype=5;profile-level-id=41;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1190");
    }

    it("builds MP4A-LATM fmtp strings") {
      turbo_rtsp_sdp_mpeg4_fmtp_t mpeg4;
      char fmtp[128];

      memset(&mpeg4, 0, sizeof(mpeg4));
      mpeg4.flags = TURBO_RTSP_SDP_MPEG4_FMTP_PROFILE_LEVEL_ID |
                    TURBO_RTSP_SDP_MPEG4_FMTP_OBJECT |
                    TURBO_RTSP_SDP_MPEG4_FMTP_CPRESENT |
                    TURBO_RTSP_SDP_MPEG4_FMTP_CONFIG;
      snprintf(mpeg4.profile_level_id, sizeof(mpeg4.profile_level_id), "9");
      mpeg4.object = 8;
      mpeg4.cpresent = 0;
      snprintf(mpeg4.config, sizeof(mpeg4.config), "9128B1071070");

      check_int_eq(
          turbo_rtsp_sdp_mpeg4_fmtp_build(fmtp, sizeof(fmtp), &mpeg4),
          (int)strlen("profile-level-id=9;object=8;cpresent=0;config=9128B1071070"));
      check_str_eq(fmtp, "profile-level-id=9;object=8;cpresent=0;config=9128B1071070");
    }

    it("builds H264 fmtp parameter set strings") {
      turbo_rtsp_sdp_h264_fmtp_t h264;
      char fmtp[160];

      memset(&h264, 0, sizeof(h264));
      h264.flags = TURBO_RTSP_SDP_H264_FMTP_PACKETIZATION_MODE |
                   TURBO_RTSP_SDP_H264_FMTP_PROFILE_LEVEL_ID |
                   TURBO_RTSP_SDP_H264_FMTP_SPROP_PARAMETER_SETS;
      h264.packetization_mode = 1;
      snprintf(h264.profile_level_id, sizeof(h264.profile_level_id), "42e01f");
      snprintf(
          h264.sprop_parameter_sets,
          sizeof(h264.sprop_parameter_sets),
          "Z0IAH5WoFAFuQA==,aM48gA==");

      check_int_eq(
          turbo_rtsp_sdp_h264_fmtp_build(fmtp, sizeof(fmtp), &h264),
          (int)strlen("packetization-mode=1;profile-level-id=42e01f;sprop-parameter-sets=Z0IAH5WoFAFuQA==,aM48gA=="));
      check_str_eq(
          fmtp,
          "packetization-mode=1;profile-level-id=42e01f;sprop-parameter-sets=Z0IAH5WoFAFuQA==,aM48gA==");
    }

    it("parses H265 fmtp parameter sets and writes Annex-B extradata") {
      static const char sdp_body[] =
          "v=0\r\n"
          "s=RTSP Session\r\n"
          "m=video 0 RTP/AVP 98\r\n"
          "a=rtpmap:98 H265/90000\r\n"
          "a=fmtp:98 profile-id=1; sprop-vps=QAE=; sprop-sps=QgE=,QgEC; sprop-pps=RAE=; sprop-sei=TgE=\r\n"
          "a=control:trackID=0\r\n";
      const uint8_t expected_annexb[] = {
          0x00, 0x00, 0x00, 0x01, 0x40, 0x01,
          0x00, 0x00, 0x00, 0x01, 0x42, 0x01,
          0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x02,
          0x00, 0x00, 0x00, 0x01, 0x44, 0x01,
          0x00, 0x00, 0x00, 0x01, 0x4e, 0x01
      };
      static turbo_rtsp_sdp_description_t parsed;
      uint8_t annexb[sizeof(expected_annexb)];
      size_t written = 0;

      memset(&parsed, 0, sizeof(parsed));
      memset(annexb, 0, sizeof(annexb));

      check_int_eq(turbo_rtsp_sdp_parse(sdp_body, sizeof(sdp_body) - 1, &parsed), 0);
      check_size_eq(parsed.media_count, 1);
      check_str_eq(parsed.media[0].encoding_name, "H265");
      check_str_eq(
          parsed.media[0].fmtp,
          "profile-id=1; sprop-vps=QAE=; sprop-sps=QgE=,QgEC; sprop-pps=RAE=; sprop-sei=TgE=");
      check_int_eq(
          (int)parsed.media[0].h265_fmtp.flags,
          (int)(TURBO_RTSP_SDP_H265_FMTP_SPROP_VPS |
                TURBO_RTSP_SDP_H265_FMTP_SPROP_SPS |
                TURBO_RTSP_SDP_H265_FMTP_SPROP_PPS |
                TURBO_RTSP_SDP_H265_FMTP_SPROP_SEI));
      check_str_eq(parsed.media[0].h265_fmtp.sprop_vps, "QAE=");
      check_str_eq(parsed.media[0].h265_fmtp.sprop_sps, "QgE=,QgEC");
      check_str_eq(parsed.media[0].h265_fmtp.sprop_pps, "RAE=");
      check_str_eq(parsed.media[0].h265_fmtp.sprop_sei, "TgE=");

      check_int_eq(
          turbo_rtsp_sdp_h265_fmtp_write_annexb(
              annexb,
              sizeof(annexb),
              &parsed.media[0].h265_fmtp,
              &written),
          0);
      check_size_eq(written, sizeof(expected_annexb));
      check_mem_eq(annexb, expected_annexb, sizeof(expected_annexb));
    }

    it("builds H265 fmtp parameter set strings") {
      turbo_rtsp_sdp_h265_fmtp_t h265;
      char fmtp[128];

      memset(&h265, 0, sizeof(h265));
      h265.flags = TURBO_RTSP_SDP_H265_FMTP_SPROP_VPS |
                   TURBO_RTSP_SDP_H265_FMTP_SPROP_SPS |
                   TURBO_RTSP_SDP_H265_FMTP_SPROP_PPS;
      snprintf(h265.sprop_vps, sizeof(h265.sprop_vps), "QAE=");
      snprintf(h265.sprop_sps, sizeof(h265.sprop_sps), "QgE=");
      snprintf(h265.sprop_pps, sizeof(h265.sprop_pps), "RAE=");

      check_int_eq(
          turbo_rtsp_sdp_h265_fmtp_build(fmtp, sizeof(fmtp), &h265),
          (int)strlen("sprop-vps=QAE=; sprop-sps=QgE=; sprop-pps=RAE="));
      check_str_eq(fmtp, "sprop-vps=QAE=; sprop-sps=QgE=; sprop-pps=RAE=");
    }

    it("rejects malformed H265 fmtp parameter sets") {
      turbo_rtsp_sdp_h265_fmtp_t h265;
      uint8_t annexb[8];
      size_t written = 99;

      memset(&h265, 0, sizeof(h265));
      check_int_eq(turbo_rtsp_sdp_h265_fmtp_parse("sprop-vps=", 0, &h265), -1);
      check_int_eq(turbo_rtsp_sdp_h265_fmtp_parse("sprop-vps", 0, &h265), -1);

      check_int_eq(turbo_rtsp_sdp_h265_fmtp_parse("sprop-vps=not-base64", 0, &h265), 0);
      check_int_eq(
          turbo_rtsp_sdp_h265_fmtp_write_annexb(
              annexb,
              sizeof(annexb),
              &h265,
              &written),
          -1);
      check_size_eq(written, 0);

      check_int_eq(turbo_rtsp_sdp_h265_fmtp_parse("sprop-vps=QAE=;sprop-sps=QgE=", 0, &h265), 0);
      check_int_eq(
          turbo_rtsp_sdp_h265_fmtp_write_annexb(
              annexb,
              5,
              &h265,
              &written),
          -1);
      check_size_eq(written, 0);
    }

    it("rejects malformed H264 fmtp parameter sets") {
      turbo_rtsp_sdp_h264_fmtp_t h264;
      uint8_t annexb[8] = {0xaa, 0xbb};
      size_t written = 99;

      memset(&h264, 0, sizeof(h264));
      check_int_eq(turbo_rtsp_sdp_h264_fmtp_parse("packetization-mode=3", 0, &h264), -1);
      check_int_eq(turbo_rtsp_sdp_h264_fmtp_parse("sprop-parameter-sets=", 0, &h264), -1);
      check_int_eq(turbo_rtsp_sdp_h264_fmtp_parse("sprop-parameter-sets", 0, &h264), -1);

      check_int_eq(turbo_rtsp_sdp_h264_fmtp_parse("sprop-parameter-sets=not-base64", 0, &h264), 0);
      check_int_eq(
          turbo_rtsp_sdp_h264_fmtp_write_annexb(
              annexb,
              sizeof(annexb),
              &h264,
              &written),
          -1);
      check_size_eq(written, 0);
      check_int_eq(annexb[0], 0xaa);
      check_int_eq(annexb[1], 0xbb);

      check_int_eq(turbo_rtsp_sdp_h264_fmtp_parse("sprop-parameter-sets=Z0IAH5WoFAFuQA==", 0, &h264), 0);
      check_int_eq(
          turbo_rtsp_sdp_h264_fmtp_write_annexb(
              annexb,
              5,
              &h264,
              &written),
          -1);
      check_size_eq(written, 0);
    }

    it("rejects malformed MPEG4 AAC fmtp config") {
      turbo_rtsp_sdp_mpeg4_fmtp_t mpeg4;
      uint8_t config[2] = {0xaa, 0xbb};
      size_t written = 99;

      memset(&mpeg4, 0, sizeof(mpeg4));
      check_int_eq(turbo_rtsp_sdp_mpeg4_fmtp_parse("config=119", 0, &mpeg4), 0);
      check_int_eq(
          turbo_rtsp_sdp_mpeg4_fmtp_write_config(
              config,
              sizeof(config),
              &mpeg4,
              &written),
          -1);
      check_size_eq(written, 0);
      check_int_eq(config[0], 0xaa);
      check_int_eq(config[1], 0xbb);

      check_int_eq(turbo_rtsp_sdp_mpeg4_fmtp_parse("config=11xz", 0, &mpeg4), 0);
      config[0] = 0xaa;
      config[1] = 0xbb;
      check_int_eq(
          turbo_rtsp_sdp_mpeg4_fmtp_write_config(
              config,
              sizeof(config),
              &mpeg4,
              &written),
          -1);
      check_size_eq(written, 0);
      check_int_eq(config[0], 0xaa);
      check_int_eq(config[1], 0xbb);

      check_int_eq(turbo_rtsp_sdp_mpeg4_fmtp_parse("config=1190", 0, &mpeg4), 0);
      check_int_eq(
          turbo_rtsp_sdp_mpeg4_fmtp_write_config(
              config,
              1,
              &mpeg4,
              &written),
          -1);
      check_size_eq(written, 0);

      check_int_eq(turbo_rtsp_sdp_mpeg4_fmtp_parse("streamtype=x;config=1190", 0, &mpeg4), -1);
      check_int_eq(turbo_rtsp_sdp_mpeg4_fmtp_parse("config", 0, &mpeg4), -1);
    }

    it("rejects malformed SDP numeric fields") {
      static const char invalid_port[] =
          "v=0\r\n"
          "s=x\r\n"
          "m=video nope RTP/AVP 96\r\n";
      static const char invalid_payload_type[] =
          "v=0\r\n"
          "s=x\r\n"
          "m=video 0 RTP/AVP 128\r\n";
      static const char invalid_rtpmap_payload_type[] =
          "v=0\r\n"
          "s=x\r\n"
          "m=video 0 RTP/AVP 96\r\n"
          "a=rtpmap:x H264/90000\r\n";
      static const char invalid_clock_rate[] =
          "v=0\r\n"
          "s=x\r\n"
          "m=video 0 RTP/AVP 96\r\n"
          "a=rtpmap:96 H264/2147483648\r\n";
      static turbo_rtsp_sdp_description_t parsed;

      check_int_eq(turbo_rtsp_sdp_parse(invalid_port, sizeof(invalid_port) - 1, &parsed), -1);
      check_int_eq(
          turbo_rtsp_sdp_parse(
              invalid_payload_type,
              sizeof(invalid_payload_type) - 1,
              &parsed),
          -1);
      check_int_eq(
          turbo_rtsp_sdp_parse(
              invalid_rtpmap_payload_type,
              sizeof(invalid_rtpmap_payload_type) - 1,
              &parsed),
          -1);
      check_int_eq(
          turbo_rtsp_sdp_parse(
              invalid_clock_rate,
              sizeof(invalid_clock_rate) - 1,
              &parsed),
          -1);
    }

    it("formats DESCRIBE response bodies as application/sdp with exact Content-Length") {
      static const char sdp_body[] =
          "v=0\r\n"
          "o=- 2890844526 2890842807 IN IP4 192.0.2.1\r\n"
          "s=RTSP Session\r\n"
          "c=IN IP4 192.0.2.1\r\n"
          "t=0 0\r\n"
          "m=video 0 RTP/AVP 96\r\n"
          "a=rtpmap:96 H264/90000\r\n"
          "a=control:trackID=0\r\n";
      const turbo_rtsp_header_t headers[] = {
          {"Content-Base", "rtsp://example.com/live/"},
          {"Cache-Control", "no-cache"}
      };
      turbo_rtsp_response_t response;
      static char buffer[2048];
      static char content_length[64];
      const char *body = NULL;
      int len = 0;

      memset(buffer, 0, sizeof(buffer));
      memset(content_length, 0, sizeof(content_length));
      memset(&response, 0, sizeof(response));
      response.status_code = 200;
      response.content_type = "application/sdp";
      response.body = sdp_body;
      response.body_len = sizeof(sdp_body) - 1;
      response.headers = headers;
      response.header_count = sizeof(headers) / sizeof(headers[0]);

      len = turbo_rtsp_format_response(
          buffer,
          sizeof(buffer),
          10,
          "TurboMedia RTSP",
          &response);
      snprintf(
          content_length,
          sizeof(content_length),
          "Content-Length: %zu\r\n",
          sizeof(sdp_body) - 1);

      check(len > 0);
      check(strstr(buffer, "RTSP/1.0 200 OK\r\n") == buffer);
      check(strstr(buffer, "Content-Type: application/sdp\r\n") != NULL);
      check(strstr(buffer, "Content-Base: rtsp://example.com/live/\r\n") != NULL);
      check(strstr(buffer, "Cache-Control: no-cache\r\n") != NULL);
      check(strstr(buffer, content_length) != NULL);
      check(strstr(buffer, "m=video 0 RTP/AVP 96\r\n") != NULL);
      check(strstr(buffer, "a=rtpmap:96 H264/90000\r\n") != NULL);

      body = strstr(buffer, "\r\n\r\n");
      check(body != NULL);
      body += 4;
      check_str_eq(body, sdp_body);
    }
  }
}

#else

suite("turbo_rtsp_lib") {
  it("is disabled when TURBO_MEDIA_HAS_RTSP is not defined") {
    check(1);
  }
}

#endif /* TURBO_MEDIA_HAS_RTSP */
