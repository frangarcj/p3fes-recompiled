/* Extra linker script for the Vita executable (-Wl,-T,link_pad.ld).
   The vitasdk linker script starts the data segment at ALIGN(0x10000) after the
   text segment, and vita-elf-create then needs ~5 KB at the end of the text
   segment for the SCE module info and NID tables. When the text ends within
   that distance below a 64 KB boundary the build fails with
   "Cannot allocate N bytes for SCE data at end of segment 0; segment 1 overlaps".
   This pads the text by 16 KB in that case. Newer vitasdk
   linker scripts offer __sce_headroom for the same purpose; the installed one
   does not. */
SECTIONS
{
  .ps2x_pad (READONLY) :
  {
    BYTE(0)
    /* Only constants are added to '.': mixing ABSOLUTE(.) into the sum makes the
       whole expression absolute and the section address wraps. 16 KB carries any
       text end within 8 KB below a 64 KB boundary (or exactly on it) past it. */
    . += (((ABSOLUTE(.) & 0xFFFF) > 0xE000) || ((ABSOLUTE(.) & 0xFFFF) == 0)) ? 0x4000 : 0;
  }
}
INSERT AFTER .eh_frame;
