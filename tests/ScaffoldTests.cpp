#include "plugin/InstrumentCore.h"

#include <array>
#include <iostream>

int main()
{
    dropforge::InstrumentCore engine;
    engine.prepare(48000.0);
    std::array<float, 1026> left{}, right{};
    // Changing block lengths include zero and single-sample host callbacks.
    for (const int samples : {0, 1, 17, 512, 1024, 3})
    {
        left.fill(1.0f);
        right.fill(-1.0f);
        engine.process(left.data() + 1, right.data() + 1, samples);
        for (int i = 0; i < samples; ++i)
        {
            if (left[static_cast<std::size_t>(i + 1)] != 0.0f
                || right[static_cast<std::size_t>(i + 1)] != 0.0f)
            {
                std::cerr << "Idle engine must emit exact silence\n";
                return 1;
            }
        }
        if (left.front() != 1.0f || right.front() != -1.0f
            || left[static_cast<std::size_t>(samples + 1)] != 1.0f
            || right[static_cast<std::size_t>(samples + 1)] != -1.0f)
        {
            std::cerr << "Processing wrote outside the requested block\n";
            return 1;
        }
    }
    return 0;
}
