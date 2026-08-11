#ifndef TURBO_MEDIA_IVR_TURBOXML_ADAPTER_H
#define TURBO_MEDIA_IVR_TURBOXML_ADAPTER_H

/**
 * @file ivr_turboxml_adapter.h
 * @brief TurboXML-backed engine adapter (CCXML + VXML dialog bridge).
 *
 * Implementation notes (verified against the installed TurboXML build):
 * - CCXML and the built-in CCXML<->VXML dialog bridge work through the C API:
 *   `<accept/>`, `<disconnect/>` and `<dialogstart>` map to platform
 *   callbacks, VXML prompts/fields map to play_prompt/collect_input.
 * - SCXML drives rtc_session.scxml: the interpreter is initialized eagerly at
 *   create (InterpreterImpl::init runs on the first step(); receive() before
 *   that crashes on a NULL event queue impl). RTC/room facts are injected as
 *   {"name": ...} and `<send target="ivr.command">` is captured via the
 *   execution plugin on_execution_point (name=="send", phase=="before"). The
 *   wrapper exposes the full send content on the exec point (event, target,
 *   type, delay and "param.<name>" expr keys); the adapter emits an
 *   ivr.command intent named after the send event with the params as command
 *   args JSON (simple quoted string literals are unquoted).
 * - Stepping constraint (deterministic, verified): Interpreter::step() blocks
 *   forever when the external event queue is empty (its default is
 *   blockMs=max -> BasicEventQueue::dequeue waits on a condvar). step() must
 *   therefore only be driven while an injected event is still being processed.
 *   The adapter tracks this with scxml_pending/scxml_event_seen/scxml_stable:
 *   it steps until an "event before" execution point has fired AND a
 *   subsequent stable configuration is reached (the microstep preamble reports
 *   stable before processing, so a bare stable does not stop the drain).
 * - VXML `<submit next="ivr://command/...">` is not surfaced by the bridge, so
 *   recognized inputs are mapped to business commands through the content
 *   package command_map (adapter-layer conversion, no private XML tags).
 */

#include "ivr_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Concrete engine ops backed by TurboXML (see ivr_xml_engine_ops_t). */
extern const ivr_xml_engine_ops_t ivr_turboxml_engine_ops;

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_TURBOXML_ADAPTER_H */
