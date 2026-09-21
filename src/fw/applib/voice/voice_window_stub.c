/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "applib/voice/voice_window.h"
#include "applib/voice/voice_window_private.h"

#include <string.h>

static VoiceUiData s_voice_window;

VoiceWindow *voice_window_create(char *buffer, size_t buffer_size,
                                 VoiceEndpointSessionType session_type) {
  (void)buffer;
  (void)buffer_size;
  (void)session_type;
  memset(&s_voice_window, 0, sizeof(s_voice_window));
  return &s_voice_window;
}

void voice_window_destroy(VoiceWindow *voice_window) {
  (void)voice_window;
}

DictationSessionStatus voice_window_push(VoiceWindow *voice_window) {
  (void)voice_window;
  return DictationSessionStatusFailureDisabled;
}

void voice_window_pop(VoiceWindow *voice_window) {
  (void)voice_window;
}

void voice_window_set_confirmation_enabled(VoiceWindow *voice_window, bool enabled) {
  (void)voice_window;
  (void)enabled;
}

void voice_window_set_error_enabled(VoiceWindow *voice_window, bool enabled) {
  (void)voice_window;
  (void)enabled;
}

void voice_window_reset(VoiceWindow *voice_window) {
  (void)voice_window;
}

void voice_window_lose_focus(VoiceWindow *voice_window) {
  (void)voice_window;
}

void voice_window_regain_focus(VoiceWindow *voice_window) {
  (void)voice_window;
}

void voice_window_transcription_dialog_keep_alive_on_select(VoiceWindow *voice_window,
                                                            bool keep_alive_on_select) {
  (void)voice_window;
  (void)keep_alive_on_select;
}
