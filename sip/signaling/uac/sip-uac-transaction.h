#ifndef _sip_uac_transaction_h_
#define _sip_uac_transaction_h_

#include "sip-uac.h"
#include "sip-timer.h"
#include "sip-message.h"
#include "sip-transport.h"
#include "sip-atomic.h"
#include "threads/threads.h"

#define UDP_PACKET_SIZE (4*1024) //1440

enum
{
	SIP_UAC_TRANSACTION_CALLING,
	SIP_UAC_TRANSACTION_TRYING = SIP_UAC_TRANSACTION_CALLING,
	SIP_UAC_TRANSACTION_PROCEEDING,
	SIP_UAC_TRANSACTION_COMPLETED,
	SIP_UAC_TRANSACTION_ACCEPTED, // rfc6026 7.2. (p9)
	SIP_UAC_TRANSACTION_TERMINATED,
};

enum { SIP_UAC_ACCEPTED_LIMIT = 16, SIP_UAC_NEXT_HOP_BYTES = 256 };

struct sip_uac_accepted_dialog
{
	struct sip_dialog_t* dialog;
	uint8_t* ack;
	int ack_size;
	char next_hop[SIP_UAC_NEXT_HOP_BYTES];
};

struct sip_agent_t;
struct sip_uac_transaction_t
{
	int linked;
	sync_mutex_t locker;
	sip_atomic_i32_t ref;

	// valid only in [sip_uas_input, sip_uas_reply]
	// create in sip_uas_input, destroy in sip_uas_reply
	struct sip_message_t* req;
	uint8_t data[UDP_PACKET_SIZE];
	int size;
	int reliable; // udp-0, other-1
	int retransmission; // default 0

	int status;
	int retries;
	int t2; // 64*T1-invite, 4s-non-invite
	sip_timer_t timera; // retransmission timer(timer E)
	sip_timer_t timerb; // timeout(timer F)
	sip_timer_t timerd; // wait for all duplicate-reply(ack) message(timer K)

	struct sip_agent_t* agent;
	struct sip_dialog_t* dialog;
	struct sip_uac_accepted_dialog accepted[SIP_UAC_ACCEPTED_LIMIT];
	unsigned accepted_count;
	int (*onhandle)(struct sip_uac_transaction_t* t, int code);
	sip_uac_onsubscribe onsubscribe;
	sip_uac_oninvite oninvite;
	sip_uac_onreply onreply;
	void* param;
    sip_transaction_ondestroy ondestroy;
    void* ondestroyparam;
	
	struct sip_transport_t transport;
	char next_hop[SIP_UAC_NEXT_HOP_BYTES];
//	sip_uac_onsend onsend;
	void* transportptr;
};

struct sip_uac_transaction_t* sip_uac_transaction_create(struct sip_agent_t* sip, struct sip_message_t* req);
//int sip_uac_transaction_addref(struct sip_uac_transaction_t* t);
//int sip_uac_transaction_release(struct sip_uac_transaction_t* t);

int sip_uac_transaction_send(struct sip_uac_transaction_t* t);

int sip_uac_transaction_invite_input(struct sip_uac_transaction_t* t, const struct sip_message_t* reply);
int sip_uac_transaction_noninvite_input(struct sip_uac_transaction_t* t, const struct sip_message_t* reply);

// calling/trying + proceeding timeout
int sip_uac_transaction_timeout(struct sip_uac_transaction_t* t, int timeout);

// wait for all inflight reply
int sip_uac_transaction_timewait(struct sip_uac_transaction_t* t, int timeout);

int sip_uac_next_hop(const struct sip_message_t* request, char next_hop[SIP_UAC_NEXT_HOP_BYTES]);
int sip_uac_select_hop(struct sip_uac_transaction_t* t, const char* next_hop);
int sip_uac_transaction_via(struct sip_uac_transaction_t* t, const char* next_hop,
                            char *via, int nvia, char *contact, int ncontact);
int sip_uac_resend_ack(struct sip_uac_transaction_t* t, const struct sip_uac_accepted_dialog* branch);

sip_timer_t sip_uac_start_timer(struct sip_agent_t* sip, struct sip_uac_transaction_t* t, int timeout, sip_timer_handle handler);
void sip_uac_stop_timer(struct sip_agent_t* sip, struct sip_uac_transaction_t* t, sip_timer_t* id);

int sip_uac_ack_3456xx(struct sip_uac_transaction_t* t, const struct sip_message_t* reply);

int sip_uac_notify_onreply(struct sip_uac_transaction_t* t, const struct sip_message_t* reply);
int sip_uac_subscribe_onreply(struct sip_uac_transaction_t* t, const struct sip_message_t* reply);

#endif /* !_sip_uac_transaction_h_ */
