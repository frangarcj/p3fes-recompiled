#pragma once

class PS2Runtime;

// Message-window (dialogue) cuts. Switches, read once at install time:
//   P3FES_CUT_FONT          frFontRaster (0x3B38F0), the per-glyph rasteriser of the text/HUD font, replaced by a native
//                           version of its steady path (default ON; "0" restores the guest function; "verify" runs the
//                           native path and the original on a RAM copy for every Nth glyph and compares the registers,
//                           the whole of guest RAM outside the dead stack frame and the Im2D vertex stream; N =
//                           P3FES_CUT_FONT_VERIFY_EVERY, default 61; diagnostic, it is slow)
//   P3FES_DIALOGUE_DIAG=1   every 5 s: calls and inclusive microseconds per frame of the message-window functions,
//                           the game thread's CPU ms per second (a deterministic cost metric on the Mac; the probes
//                           themselves add ~0.05 us per call)
// The "[p3fes-dlg-font]" counter line is printed with P3FES_RESOURCE_DIAG=1 (through p3fes_native_frame_cuts::logCounters).
namespace p3fes_native_dialogue
{
    // Returns true when at least one hook was installed.
    bool install(PS2Runtime &runtime);

    // Periodic counter line.
    void logCounters();
}
