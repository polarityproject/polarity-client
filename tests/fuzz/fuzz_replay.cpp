// libFuzzer target: a recording read from any bytes (scripts/fuzz.ps1).
#include "core/replay_file.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    sfv::ReplayFile rf;
    std::string error;
    sfv::DeserializeReplay(data, size, &rf, &error);
    return 0;
}
