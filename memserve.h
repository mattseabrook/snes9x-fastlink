/*****************************************************************************\
	 Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
				This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifdef snprintf
    #undef snprintf
#endif

#include "json.hpp"

#define snprintf _snprintf

using json = nlohmann::json;

void MemServe();

inline json ramToJson(uint8_t* RAM, size_t size) {
    json j;
    for (size_t i = 0; i < size; ++i) {
        char buffer[3];
        sprintf(buffer, "%02x", RAM[i]);
        j["RAM"].push_back(buffer);
    }
    return j;
}