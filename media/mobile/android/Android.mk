# TurboMedia Android staging sources.
#
# This file is intentionally not a build script for the current repository.
# The imported Android sources still depend on the older TurboWebRTC media,
# codec, RTP, recorder, and media-engine APIs. Adapt those sources to the
# current TurboMedia public API before enabling ndk-build support here.

$(error TurboMedia Android mobile sources are staged only; Android.mk is not enabled)
