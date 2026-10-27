#pragma once

class PS2Runtime;

// Empty message texts. cMessDrawFont_measureText (0x2B0700) walks a text's 16-bit codes until a page-end code; a
// text buffer that holds none (all zeros) sends it through memory for good, on a worker thread, while the main
// thread waits on the task's semaphore: the game sits at 0 FPS. Seen (Oct 27) loading a save from the options menu
// after a save state load: messages 0x3041 / 0x3042 (the memory card slots' texts, the game points them at static
// 0x100-byte buffers) were still all zeros. cMessDrawFont_setMessage (0x2AF590, a0 = the drawer, a1 = the id) is
// watched: an all-zero buffer gets a page-end code first, so the message draws as empty.
namespace kamite::message_guard
{
    void install(PS2Runtime &runtime);
}
