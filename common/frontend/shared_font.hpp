#pragma once

#include <switch.h>

namespace nxmusic {

inline Result getStandardFont(PlFontData& font)
{
    constexpr u32 standard = PlSharedFontType_Standard;
    constexpr u32 loaded = 1;
    constexpr int maxAttempts = 60;
    Service* session = plGetServiceSession();
    u32 state = 0;

    // libnx's font lookup polls indefinitely if the requested font never
    // loads. Check readiness first, then let libnx validate its address.
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        const Result check = serviceDispatchInOut(session, 1, standard, state);
        if (R_FAILED(check)) return check;
        if (state == loaded)
            return plGetSharedFontByType(&font, PlSharedFontType_Standard);

        if (attempt == 0) {
            const Result request = serviceDispatchIn(session, 0, standard);
            if (R_FAILED(request)) return request;
        }
        if (attempt + 1 < maxAttempts) svcSleepThread(16'666'667);
    }
    return KERNELRESULT(TimedOut);
}

}
