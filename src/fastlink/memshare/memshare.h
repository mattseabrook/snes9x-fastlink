/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef MEMSHARE_H
#define MEMSHARE_H

#include <cstddef>
#include <cstdint>

bool MemShareStart();
void MemShareStop();
bool MemShareIsRunning();
void MemSharePublish(const uint8_t *data, size_t bytes, uint64_t frameId = 0, uint64_t emuQpc = 0);

#endif // MEMSHARE_H
