#include "WifiConnector.h"

// Single definition of the RTC-persisted clock snapshot declared extern in the
// header (header-only class → vars must live in exactly one TU). RTC_NOINIT_ATTR
// places them in RTC slow memory: retained across a SW reset, lost on power loss
// (then the magic won't match and RestoreClockFromRtc() skips). See WifiConnector.h.
#ifdef LIONWIFI_RTC_CLOCK
#include <esp_attr.h>
RTC_NOINIT_ATTR uint32_t _lwRtcSavedEpoch;
RTC_NOINIT_ATTR uint32_t _lwRtcSavedMagic;
#endif
