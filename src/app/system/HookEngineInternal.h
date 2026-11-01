// HookEngineInternal.h
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "core/Logger.h"
#include <cassert>

#define HOOK_LOG(fmt, ...) do {                                              \
    if (::NextKey::Logger::IsEnabled())                                      \
        ::NextKey::Logger::Log(L"[Hook] " fmt, ##__VA_ARGS__);               \
} while (0)

#if defined(NDEBUG) || !defined(_WIN32)
  #define VKEY_ASSERT_HOOK_THREAD() ((void)0)
#else
  #define VKEY_ASSERT_HOOK_THREAD()                                            \
      do {                                                                     \
          const DWORD _expected = lifecycle_.ThreadId();                        \
          if (_expected != 0) {                                                 \
              const DWORD _current = GetCurrentThreadId();                      \
              if (_current != _expected) {                                      \
                  HOOK_LOG(L"VKEY_ASSERT_HOOK_THREAD violated: tid=%lu, expected hook tid=%lu — %hs", \
                           _current, _expected, __func__);                      \
                  assert(_current == _expected &&                               \
                         "composition-state mutation must run on the hook thread"); \
              }                                                                 \
          }                                                                     \
      } while (0)
#endif
