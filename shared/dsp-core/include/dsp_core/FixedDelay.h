#pragma once
#include <array>
#include <cstddef>
#include <type_traits>

namespace dsp_core
{
// Owns every sample: copied engines and changing callback sizes retain history.
template <typename T, std::size_t Samples>
class FixedDelay
{
    static_assert(Samples > 0, "Use direct assignment for zero delay");
    static_assert(std::is_trivially_copyable_v<T>, "Audio delay values must not own dynamic storage");
public:
    void reset() noexcept { history.fill(T{}); position = 0; }
    T next(const T& input) noexcept
    {
        const auto output = history[position];
        history[position] = input;
        if (++position == Samples) position = 0;
        return output;
    }
private:
    std::array<T, Samples> history{};
    std::size_t position = 0;
};
}
