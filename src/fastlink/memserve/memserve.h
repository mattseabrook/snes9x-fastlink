/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef MEMSERVE_H
#define MEMSERVE_H

// Start the RAM streaming server (blocking, run in a thread)
void MemServe();

// Signal the server to stop and close listen socket (call from main thread)
void MemServeStop();

#endif // MEMSERVE_H