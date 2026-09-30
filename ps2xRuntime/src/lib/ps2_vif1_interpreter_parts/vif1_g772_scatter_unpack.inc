// Run existing decode kernels directly on source-page spans. A V4_32 UNPACK
// often occupies 4096 bytes plus its command: assembling that entire command
// whenever it crosses a page would silently recreate the eliminated copy.
// At most one vector is materialized at each fragment boundary.
static void g772RunScatterUnpack(G650UnpackKind kind, uint8_t* dst, const uint8_t*& data,
                                uint32_t offset, uint32_t vectors, uint32_t stride,
                                uint32_t components, bool zeroExtend)
{
    // Preserve the canonical copied arm's one-kernel call, including its cost.
    g650RunUnpackKernel(kind, dst, data + offset, vectors, stride, components, zeroExtend);
}

template<class Input>
static void g772RunScatterUnpack(G650UnpackKind kind, uint8_t* dst, Input& data,
                                uint32_t offset, uint32_t vectors, uint32_t stride,
                                uint32_t components, bool zeroExtend)
{
    while (vectors)
    {
        const size_t available = ps2_dma::contiguous(data + offset);
        const uint32_t count = available >= stride
            ? static_cast<uint32_t>(std::min<size_t>(vectors, available / stride)) : 1u;
        const auto* src = ps2_dma::borrow(data + offset, count * stride);
        g650RunUnpackKernel(kind, dst, src, count, stride, components, zeroExtend);
        dst += count * 16u;
        offset += count * stride;
        vectors -= count;
    }
}
