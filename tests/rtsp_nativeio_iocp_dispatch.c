/* M15d/#192: genuine installed-Salts CNet RTSP Server versus a PRIVATE
 * source copy of the original NativeIO Windows IOCP backend.
 *
 * A real GetQueuedCompletionStatus() OVERLAPPED packet is dequeued by
 * this consumer's one NativeIO Owner. The upstream Salts #1168 TEST-ONLY
 * hook then calls cancel BEFORE publishing the one original terminal.
 * Production CNet/NativeIO and TurboMedia::RTSP have NO test hook or
 * changed ABI; this is not a fabricated Windows completion.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <turbo_rtsp.h>
#include <salts/native_io.h>
#include <salts/error_codes.h>
#include "native_io_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    M15D_PORT=21578, M15D_ROUNDS=12, M15D_TIMEOUT_MS=5000,
    M15D_IOCP_DEQUEUED_CANCEL=1, M15D_CANCEL_WHILE_SERVER_HELD=2
};

typedef struct m15d_rtsp_fixture {
    volatile LONG calls;
    volatile LONG closes;
    volatile LONG entered;
    volatile LONG release;
    volatile LONG exited;
    volatile LONG wrong_cseq;
    unsigned expected_cseq; /* Immutable while server callback runs. */
    int hold_callback;
} m15d_rtsp_fixture;

typedef struct m15d_iocp_gate {
    native_io_backend *owner;
    native_io_request request;
    native_io_endpoint endpoint;
    unsigned armed;
    unsigned entered;
    unsigned wrong_request;
    int cancel_status;
    int retained_status;
} m15d_iocp_gate;

static m15d_iocp_gate gate;

static LONG read_flag(volatile LONG *value) {
    return InterlockedCompareExchange(value, 0, 0);
}

static int await_flag(volatile LONG *value) {
    const ULONGLONG start=GetTickCount64();
    while (read_flag(value)==0) {
        if (GetTickCount64()-start>=M15D_TIMEOUT_MS)
            return 0;
        SwitchToThread();
    }
    return 1;
}

static int rtsp_options(
    turbo_rtsp_session_t *session,const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,void *context) {
    m15d_rtsp_fixture *fixture=(m15d_rtsp_fixture *)context;
    (void)session;
    if (!request || request->cseq!=fixture->expected_cseq)
        InterlockedIncrement(&fixture->wrong_cseq);
    InterlockedIncrement(&fixture->calls);
    if (fixture->hold_callback) {
        const ULONGLONG start=GetTickCount64();
        InterlockedExchange(&fixture->entered,1);
        while (!read_flag(&fixture->release)) {
            if (GetTickCount64()-start>=M15D_TIMEOUT_MS) {
                InterlockedIncrement(&fixture->wrong_cseq);
                break;
            }
            SwitchToThread();
        }
        InterlockedExchange(&fixture->exited,1);
    }
    return turbo_rtsp_response_options(response,NULL);
}

static void rtsp_close(turbo_rtsp_session_t *session,void *context) {
    m15d_rtsp_fixture *fixture=(m15d_rtsp_fixture *)context;
    (void)session;
    InterlockedIncrement(&fixture->closes);
}

/* Directly after a genuine GQCS OVERLAPPED completion, on this same
 * NativeIO Owner and before the matching terminal publication. */
void salts_native_io_test_before_iocp_dispatch(
    cmeta_io_impl *owner,native_io_request request) {
    if (!gate.armed || !gate.owner || gate.owner->impl!=owner)
        return;
    if (request.slot!=gate.request.slot ||
        request.generation!=gate.request.generation) {
        ++gate.wrong_request;
        return;
    }
    ++gate.entered;
    if (gate.entered!=1u) return;
    gate.cancel_status=native_io_backend_cancel(gate.owner,request);
    gate.retained_status=
        native_io_backend_release_socket(gate.owner,gate.endpoint);
}

static SOCKET connect_real_rtsp(void) {
    struct sockaddr_in addr={0};
    SOCKET fd=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,NULL,0,
                         WSA_FLAG_OVERLAPPED);
    if (fd==INVALID_SOCKET)
        return INVALID_SOCKET;
    addr.sin_family=AF_INET;
    addr.sin_port=htons(M15D_PORT);
    addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (connect(fd,(const struct sockaddr *)&addr,(int)sizeof(addr))
        ==SOCKET_ERROR) {
        (void)closesocket(fd);
        return INVALID_SOCKET;
    }
    return fd;
}

static int fail(unsigned line,unsigned round,
                const char *expression,int actual) {
    fprintf(stderr,
        "M15d REAL RTSP IOCP dequeue/cancel round=%u line=%u: %s (%d)\n",
        round,line,expression,actual);
    return 1;
}

#define CHECK(expr) do { \
    if(!(expr)) { status=fail(__LINE__,round,#expr,0);goto end_round; } \
} while(0)
#define CHECK_STATUS(expr,expected) do { \
    const int rc_=(expr); \
    if(rc_!=(expected)) {status=fail(__LINE__,round,#expr,rc_);goto end_round;} \
} while(0)

int main(void) {
    WSADATA wsa={0};
    native_io_backend owner={0};
    native_io_backend_config config={0};
    native_io_endpoint prior_endpoint={0};
    native_io_request prior_request={0};
    unsigned round;
    int result;
    if (WSAStartup(MAKEWORD(2,2),&wsa)!=0)
        return fail(__LINE__,0u,"WSAStartup",WSAGetLastError());

    config.kind=NATIVE_IO_BACKEND_IOCP;
    config.endpoint_capacity=1u;
    config.request_capacity=1u;
    config.completion_batch_capacity=1u;
    result=native_io_backend_init(&owner,&config);
    if (result!=SALTS_OK) {
        (void)WSACleanup();
        return fail(__LINE__,0u,"NativeIO IOCP init",result);
    }
    for(round=0u;round<M15D_ROUNDS;++round) {
        const unsigned mode=round%3u;
        m15d_rtsp_fixture fixture={0};
        turbo_rtsp_server_handlers_t callbacks={0};
        turbo_rtsp_server_config_t scfg={0};
        turbo_rtsp_server_t *server=NULL;
        SOCKET fd=INVALID_SOCKET;
        native_io_endpoint endpoint={0};
        native_io_request request={0};
        native_io_operation op={0};
        native_io_completion completion={0};
        char options[256];
        unsigned char buffer[2048];
        size_t count=0u;
        int attached=0,request_live=0,status=0;
        int length;
        const unsigned cseq=7000u+round;

        fixture.expected_cseq=cseq;
        fixture.hold_callback=(mode==M15D_CANCEL_WHILE_SERVER_HELD);
        callbacks.on_options=rtsp_options;
        callbacks.on_session_close=rtsp_close;
        scfg.bind_host="127.0.0.1";
        scfg.port=M15D_PORT;
        scfg.client_timeout_ms=M15D_TIMEOUT_MS;
        scfg.control_transport=TURBO_RTSP_CONTROL_TRANSPORT_TCP;
        scfg.connection_capacity=1u;
        scfg.send_queue_capacity=8u;
        scfg.send_queue_bytes=512u*1024u;
        server=turbo_rtsp_server_create(&scfg,&callbacks,&fixture);
        CHECK(server!=NULL);
        CHECK(turbo_rtsp_server_start(server)==0);
        fd=connect_real_rtsp();
        CHECK(fd!=INVALID_SOCKET);
        CHECK_STATUS(native_io_backend_attach_socket(
                         &owner,(uintptr_t)fd,&endpoint),SALTS_OK);
        attached=1;
        if(round) {
            CHECK(endpoint.slot==prior_endpoint.slot);
            CHECK(endpoint.generation!=prior_endpoint.generation);
        }
        memset(buffer,0xe5,sizeof(buffer));
        op.kind=NATIVE_IO_OPERATION_STREAM_RECV;
        op.endpoint=endpoint;
        op.buffer=buffer;
        op.length=sizeof(buffer);
        op.user_data=(uintptr_t)(0x4D150000u+round);
        CHECK_STATUS(native_io_backend_submit(&owner,&op,&request),SALTS_OK);
        request_live=1;
        if(round) {
            CHECK(request.slot==prior_request.slot);
            CHECK(request.generation!=prior_request.generation);
            CHECK_STATUS(native_io_backend_cancel(&owner,prior_request),SALTS_ENOENT);
        }
        memset(&gate,0,sizeof(gate));
        gate.owner=&owner;
        gate.endpoint=endpoint;
        gate.request=request;
        gate.armed=(mode==M15D_IOCP_DEQUEUED_CANCEL);

        length=snprintf(options,sizeof(options),
            "OPTIONS rtsp://127.0.0.1:%u/m15d/iocp RTSP/1.0\r\n"
            "CSeq: %u\r\n\r\n",(unsigned)M15D_PORT,cseq);
        CHECK(length>0 && (size_t)length<sizeof(options));
        CHECK(send(fd,options,length,0)==length);
        if(mode==M15D_CANCEL_WHILE_SERVER_HELD) {
            /* A genuine RTSP CNet Server callback is blocked on-stack;
             * no response can be queued when this same IOCP Owner cancels. */
            CHECK(await_flag(&fixture.entered));
            CHECK_STATUS(native_io_backend_cancel(&owner,request),SALTS_OK);
            CHECK_STATUS(native_io_backend_release_socket(&owner,endpoint),
                         SALTS_EBUSY);
            InterlockedExchange(&fixture.release,1);
            CHECK(await_flag(&fixture.exited));
        }
        CHECK_STATUS(native_io_backend_observe(
                         &owner,&completion,1u,M15D_TIMEOUT_MS,&count),SALTS_OK);
        request_live=0;
        CHECK(count==1u);
        CHECK(completion.request.slot==request.slot &&
              completion.request.generation==request.generation);
        CHECK(completion.endpoint.slot==endpoint.slot &&
              completion.endpoint.generation==endpoint.generation);
        CHECK(completion.user_data==op.user_data);
        if(mode==M15D_CANCEL_WHILE_SERVER_HELD) {
            CHECK(completion.kind==NATIVE_IO_COMPLETION_CANCELLED);
            CHECK(completion.status==SALTS_ECANCELED);
            CHECK(buffer[0]==0xe5u);
            CHECK(gate.entered==0u);
        } else {
            CHECK(completion.kind==NATIVE_IO_COMPLETION_OK);
            CHECK(completion.status==SALTS_OK);
            CHECK(completion.bytes>0u && buffer[0]=='R');
            if(mode==M15D_IOCP_DEQUEUED_CANCEL) {
                CHECK(gate.entered==1u && gate.wrong_request==0u);
                CHECK(gate.cancel_status==SALTS_EALREADY);
                CHECK(gate.retained_status==SALTS_EBUSY);
            } else {
                CHECK(gate.entered==0u);
            }
        }
        CHECK(read_flag(&fixture.wrong_cseq)==0);
        CHECK(await_flag(&fixture.calls));
        gate.armed=0u;
        CHECK_STATUS(native_io_backend_cancel(&owner,request),SALTS_ENOENT);
        count=9u;
        CHECK_STATUS(native_io_backend_observe(
                         &owner,&completion,1u,0u,&count),SALTS_ETIMEDOUT);
        CHECK(count==0u);
        prior_endpoint=endpoint;
        prior_request=request;

end_round:
        InterlockedExchange(&fixture.release,1);
        gate.armed=0u;
        if(request_live) {
            (void)native_io_backend_cancel(&owner,request);
            count=0u;
            (void)native_io_backend_observe(
                &owner,&completion,1u,M15D_TIMEOUT_MS,&count);
        }
        if(fd!=INVALID_SOCKET) (void)closesocket(fd);
        if(attached) {
            result=native_io_backend_release_socket(&owner,endpoint);
            if(result!=SALTS_OK && !status)
                status=fail(__LINE__,round,"release after original terminal",result);
        }
        if(server) {
            turbo_rtsp_server_stop(server);
            turbo_rtsp_server_destroy(server);
        }
        if(status) {
            (void)native_io_backend_close(&owner);
            (void)native_io_backend_destroy(&owner);
            (void)WSACleanup();
            return status;
        }
    }
    result=native_io_backend_close(&owner);
    if(result!=SALTS_OK)
        return fail(__LINE__,M15D_ROUNDS,"NativeIO close",result);
    result=native_io_backend_destroy(&owner);
    if(result!=SALTS_OK)
        return fail(__LINE__,M15D_ROUNDS,"NativeIO destroy",result);
    (void)WSACleanup();
    printf("M15d genuine RTSP/CNet plus IOCP dequeued Owner cancel: "
           "%u request-generation rounds PASS\n",(unsigned)M15D_ROUNDS);
    return 0;
}
