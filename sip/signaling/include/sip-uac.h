#ifndef _sip_uac_h_
#define _sip_uac_h_

#include "sip-string-view.h"
#include "sip-agent.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct sip_transport_t;
struct sip_uac_transaction_t;

/// A distinct accepted dialog is delivered once for each To tag, up to 16
/// accepted branches. The response and id are borrowed during the callback.
/// Retain the dialog if used later.
/// @param[out] session user-defined session-id(only code=2xx)
/// @return 0-ok, other-error
typedef int (*sip_uac_oninvite)(void* param, const struct sip_message_t* reply, struct sip_uac_transaction_t* t, struct sip_dialog_t* dialog, const vstr* id, int code);
/// @param[in] subscribe MUST call sip_subscribe_remove on close
/// @param[out] session user-defined session-id(only code=2xx)
/// @return 0-ok, other-error
typedef int (*sip_uac_onsubscribe)(void* param, const struct sip_message_t* reply, struct sip_uac_transaction_t* t, struct sip_subscribe_t* subscribe, const vstr* id, int code);
/// @return 0-ok, other-error
typedef int (*sip_uac_onreply)(void* param, const struct sip_message_t* reply, struct sip_uac_transaction_t* t, int code);
/// @return <0-error, 0-udp, 1-tcp, other-reserved
//typedef int (*sip_uac_onsend)(void* param, const char* url, const void* data, int bytes);

/// @param[in] name such as: "Alice <sip:alice@atlanta.com>"
/// @param[in] registrar register server, such as sip:registrar.biloxi.com. can be null.
/// @param[in] seconds expires seconds
struct sip_uac_transaction_t* sip_uac_register(struct sip_agent_t* sip, const char* name, const char* registrar, int seconds, sip_uac_onreply onregister, void* param);
struct sip_uac_transaction_t* sip_uac_options(struct sip_agent_t* sip, const char* from, const char* to, sip_uac_onreply onoptions, void* param);

struct sip_uac_transaction_t* sip_uac_invite(struct sip_agent_t* sip, const char* name, const char* to, sip_uac_oninvite oninvite, void* param);
struct sip_uac_transaction_t* sip_uac_cancel(struct sip_agent_t* sip, struct sip_uac_transaction_t* invite, sip_uac_onreply oncancel, void* param);
struct sip_uac_transaction_t* sip_uac_bye(struct sip_agent_t* sip, struct sip_dialog_t* dialog, sip_uac_onreply onbye, void* param);
struct sip_uac_transaction_t* sip_uac_reinvite(struct sip_agent_t* sip, struct sip_dialog_t* dialog, sip_uac_oninvite oninvite, void* param);
/// @param[in] req100rel sip 180/183 response with Require: 100rel
struct sip_uac_transaction_t* sip_uac_prack(struct sip_agent_t* sip, const struct sip_message_t* req100rel, struct sip_dialog_t* dialog, sip_uac_onreply onreply, void* param);
struct sip_uac_transaction_t* sip_uac_update(struct sip_agent_t* sip, struct sip_dialog_t* dialog, sip_uac_onreply onreply, void* param);
/// @param[in] dialog exist dialog(by invite), if dialog is not null, ignore from/to parameter
struct sip_uac_transaction_t* sip_uac_info(struct sip_agent_t* sip, struct sip_dialog_t* dialog, const char* package, sip_uac_onreply oninfo, void* param);


/// @param[in] from valid only dialog is null
/// @param[in] to valid only dialog is null
/// @param[in] event subscribe event type
/// @param[in] expires subscribe duration in sec, 0-unsubscribe
struct sip_uac_transaction_t* sip_uac_subscribe(struct sip_agent_t* sip, const char* from, const char* to, const char* event, int expires, sip_uac_onsubscribe onsubscribe, void* param);
struct sip_uac_transaction_t* sip_uac_notify(struct sip_agent_t* sip, struct sip_subscribe_t* subscribe, const char* state, sip_uac_onreply onnotify, void* param);
/// @param[in] expires subscribe duration in ms, 0-unsubscribe
struct sip_uac_transaction_t* sip_uac_resubscribe(struct sip_agent_t* sip, struct sip_subscribe_t* subscribe, int expires, sip_uac_onsubscribe onsubscribe, void* param);
struct sip_uac_transaction_t* sip_uac_publish(struct sip_agent_t* sip, const char* from, const char* to, const char* event, sip_uac_onreply onreply, void* param);

struct sip_uac_transaction_t* sip_uac_refer(struct sip_agent_t* sip, const char* from, const char* to, sip_uac_onreply onreply, void* param);
struct sip_uac_transaction_t* sip_uac_message(struct sip_agent_t* sip, const char* from, const char* to, sip_uac_onreply onmsg, void* param);

int sip_uac_transaction_ondestroy(struct sip_uac_transaction_t* t, sip_transaction_ondestroy ondestroy, void* param);
int sip_uac_add_header(struct sip_uac_transaction_t* t, const char* name, const char* value);
int sip_uac_add_header_int(struct sip_uac_transaction_t* t, const char* name, int value);

/// @param[in] t sip uac transaction, create by sip_uac_invite/sip_uac_register/...
/// @param[in] data message payload(such as SDP), maybe NULL if don't need send anything
/// @param[in] bytes data length in byte, >=0 only
/// @param[in] transport callbacks copied during this call
/// @param[in] param transport context, borrowed through transaction termination
/// Send once per transaction. On failure the transaction ceases transport and
/// response callbacks. Retransmission and timeout callbacks may run on SIP timer
/// threads. Transport callbacks must not reenter the agent.
/// @return 0-ok, other-error
int sip_uac_send(struct sip_uac_transaction_t* t, const void* data, int bytes, struct sip_transport_t* transport, void* param);

/// ACK the first accepted dialog. Call after the receive callback returns.
/// @param[in] invite uac transaction, create by sip_uac_invite
/// @param[in] data message payload(such as SDP), maybe NULL if don't need send anything
/// @param[in] bytes data length in byte, >=0 only
/// @param[in] ctype content-type header
/// @return 0-ok, other-error
int sip_uac_ack(struct sip_uac_transaction_t* invite, const void* data, int bytes, const char* ctype);

/* ACK this specific accepted branch outside callbacks. The transaction
 * caches the ACK for retransmissions of that branch's 2xx. */
int sip_uac_ack_dialog(struct sip_uac_transaction_t* invite, const struct sip_dialog_t* dialog,
                       const void* data, int bytes, const char* content_type);

int sip_uac_transaction_addref(struct sip_uac_transaction_t* t);
int sip_uac_transaction_release(struct sip_uac_transaction_t* t);

/* Stop a local transaction and wait for its active send/reply callback, if any.
 * No SIP message is sent. Keep a reference during this call. Do not call from
 * a SIP callback. The final ondestroy notification follows the last reference. */
void sip_uac_transaction_abort(struct sip_uac_transaction_t* t);

#if defined(__cplusplus)
}
#endif
#endif /* !_sip_uac_h_ */
