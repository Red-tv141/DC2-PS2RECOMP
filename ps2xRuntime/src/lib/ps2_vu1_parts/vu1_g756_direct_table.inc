// Address formation in the native dispatch chain must not cross an out-of-line
// container accessor. The shipping PGO/size build outlined std::array::operator[]:
// PC -> index -> accessor call/return -> slot load -> native-entry predicate.
// Keep the same fixed, contiguous storage and copy semantics, with a guaranteed
// inline address operation. These tables remain authoritative memory, not a new
// host-register allocation scheme.
#include <array>
#if defined(_MSC_VER)
#define G756_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define G756_INLINE inline __attribute__((always_inline))
#else
#define G756_INLINE inline
#endif

template<class T, size_t N>
struct G756DirectTable
{
    T elements[N];
    G756_INLINE constexpr T &operator[](size_t i) noexcept { return elements[i]; }
    G756_INLINE constexpr const T &operator[](size_t i) const noexcept { return elements[i]; }
    G756_INLINE constexpr T *data() noexcept { return elements; }
    G756_INLINE constexpr const T *data() const noexcept { return elements; }
    G756_INLINE constexpr size_t size() const noexcept { return N; }
    G756_INLINE constexpr T *begin() noexcept { return elements; }
    G756_INLINE constexpr const T *begin() const noexcept { return elements; }
    G756_INLINE constexpr T *end() noexcept { return elements + N; }
    G756_INLINE constexpr const T *end() const noexcept { return elements + N; }
    void fill(const T &value) { std::fill_n(elements, N, value); }
};

static_assert(sizeof(G756DirectTable<uint8_t, PS2_VU1_CODE_SIZE / 8u>) ==
              sizeof(std::array<uint8_t, PS2_VU1_CODE_SIZE / 8u>));
