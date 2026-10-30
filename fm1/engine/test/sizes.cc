// The RAM the engine's objects take in this build (sizeof, bytes).
#include <stdio.h>

#include "../export.h"

using namespace orb;

int main()
{
#define S(x) printf("  %-22s %8zu\n", #x, sizeof(x))
    printf("real = %s, POOL_NODES %u, MERGE_CHUNK %u, MAX_FRAMES %u, delay cell %zu B\n",
           sizeof(real) == 4 ? "float" : "double", POOL_NODES, MERGE_CHUNK, MAX_FRAMES, sizeof(*ProcessState().delay.buf));
    S(Engine);
    S(Frame);
    S(Sequencer);
    S(Pool);
    S(MergeBuf);
    S(Track);
    S(DeviceSet);
    S(MixerDevice);
    S(Tb303);
    S(Tr808);
    S(Tr909);
    S(Delay);
    S(Distortion);
    S(Pcf);
    S(Compressor);
    S(ProcessState);
    printf("  %-22s %8zu\n", "delay line cells", DELAY_LEN * sizeof(*ProcessState().delay.buf));
    S(Tables);
    S(SampleSet);
    S(SampleCursor);
    printf("  %-22s %8zu\n", "engine + delay line", sizeof(Engine) + DELAY_LEN * sizeof(*ProcessState().delay.buf));
    return 0;
}
