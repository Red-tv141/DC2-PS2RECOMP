            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;
            const bool g22IsV432 = (opcode == 0x6Cu);
            const bool g22IsLargeUnpack = (writeVectorCount >= 64u);
            if (g22IsV432)
                ++g22V432Unpacks;
            if (g22IsLargeUnpack)
                ++g22LargeUnpacks;

            // G17: for big model UNPACKs, log sizing + scan the consumed span for a VU1 kick
            // (0x14/0x15/0x17) that would indicate our source-size is too large (fill-mode
            // desync swallows the kick after the vertices).
            if (vu1_trace_enabled() && g22IsV432)
                s_f517V432Unpack.fetch_add(1u, std::memory_order_relaxed);
            if (vu1_trace_enabled() && writeVectorCount >= 64u)
            {
                static std::atomic<uint32_t> s_g17unpk{0};
                if (s_g17unpk.fetch_add(1u, std::memory_order_relaxed) < 24u)
                {
                    uint32_t firstKickOff = 0xFFFFFFFFu, firstKickWord = 0u;
                    for (uint32_t q = 4u; q + 4u <= totalBytes && pos + q + 4u <= sizeBytes; q += 4u)
                    {
                        uint32_t kw = 0u; std::memcpy(&kw, data + pos + q, 4);
                        const uint8_t kop = static_cast<uint8_t>((kw >> 24) & 0x7Fu);
                        if ((kop == 0x14u || kop == 0x15u || kop == 0x17u) && (kw & 0x00FFFFFFu) == 0u)
                        { firstKickOff = q; firstKickWord = kw; break; }
                    }
                    uint32_t nextW = 0u;
                    if (pos + totalBytes + 4u <= sizeBytes) std::memcpy(&nextW, data + pos + totalBytes, 4);
                    std::fprintf(stderr,
                                 "[G17:unpk] cmdPos=0x%x op=0x%02x num=%u cl=%u wl=%u srcVec=%u totalBytes=%u nextWord=0x%08x kickInSpan@%d=0x%08x\n",
                                 pos - 4u, static_cast<uint32_t>(opcode), writeVectorCount,
                                 cl, wl, sourceVectorCount, totalBytes, nextW,
                                 (firstKickOff == 0xFFFFFFFFu) ? -1 : static_cast<int>(firstKickOff),
                                 firstKickWord);
                }
            }

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;
            g39_seq("UNPK", vuAddr, writeVectorCount, (imm & 0x8000u) ? 1u : 0u);
            // G238: capture the map per-vertex COLOUR array's SOURCE bytes. Colour is unpacked as
            // V4-8 (vn=3, vl=2 => 4 bytes/vec, opcode 0x6E). Gated DC2_G238_COLSRC + the G138 map
            // scene gate; dump the first few source vectors so we can see whether the EE data is
            // already white (255,255,255,128 => EE build bug) or teal (unpack-scale bug).
            {
                static const bool s_g238col = (std::getenv("DC2_G238_COLSRC") != nullptr);
                extern std::atomic<bool> g_dc2G138DumpGateOpen;
                // Only V4 unpacks (vn==3) are candidate colour/pos/st streams; log all vl formats
                // with the first vector decoded both as bytes and as f32 so the colour stream
                // (values 0/128/255) is identifiable regardless of pack width.
                if (s_g238col && writeVectorCount >= 16u &&
                    g_dc2G138DumpGateOpen.load(std::memory_order_relaxed))
                {
                    static std::atomic<uint32_t> s_g238u{0};
                    const uint32_t un = s_g238u.fetch_add(1u, std::memory_order_relaxed);
                    if (un < 12u && pos + bytesPerVector * 3u <= sizeBytes)
                    {
                        const uint32_t nv = (writeVectorCount < 12u) ? writeVectorCount : 12u;
                        std::fprintf(stderr, "[G238:colsrc] u=%u vn=%u vl=%u bpv=%u dest=0x%x num=%u v:",
                                     un, (uint32_t)vn, (uint32_t)vl, bytesPerVector, vuAddr, writeVectorCount);
                        for (uint32_t v = 0; v < nv; ++v)
                        {
                            const uint32_t o = pos + v * bytesPerVector;
                            if (vl == 0u) { float f[4]; std::memcpy(f, data+o, 16); std::fprintf(stderr, " (%.0f,%.0f,%.0f,%.0f)", f[0],f[1],f[2],f[3]); }
                            else if (vl == 1u) { int16_t s[4]; std::memcpy(s, data+o, 8); std::fprintf(stderr, " (%d,%d,%d,%d)", s[0],s[1],s[2],s[3]); }
                            else { std::fprintf(stderr, " (%u,%u,%u,%u)", data[o],data[o+1],data[o+2],data[o+3]); }
                        }
                        std::fprintf(stderr, "\n"); std::fflush(stderr);
                    }
                }
            }
            if (g136Active && g136LoggedPayloads < g136_command_limit())
            {
                ++g136LoggedPayloads;
                const uint32_t lastDest = vuAddr + ((writeVectorCount > 0u) ? (writeVectorCount - 1u) : 0u);
                std::fprintf(stderr,
                             "[G136:unpack] seq=%u cmdOff=0x%x srcOff=0x%x op=0x%02x imm=0x%04x dest=0x%x..0x%x rel=%u num=%u srcVec=%u bytes=%u vn=%u vl=%u bpv=%u cl=%u wl=%u mode=%u tops=0x%x\n",
                             g136Seq, pos - 4u, pos, static_cast<uint32_t>(opcode), imm,
                             vuAddr, lastDest & 0x3FFu, (imm & 0x8000u) ? 1u : 0u,
                             writeVectorCount, sourceVectorCount, totalBytes,
                             static_cast<uint32_t>(vn), static_cast<uint32_t>(vl),
                             bytesPerVector, cl, wl, vif1_regs.mode & 3u,
                             vif1_regs.tops & 0x3FFu);
                g136_log_qwords("unpacksrc", g136Seq, pos, data + pos,
                                (sizeBytes > pos) ? (sizeBytes - pos) : 0u,
                                totalBytes / 16u);
            }
            // G62: localize EE-vs-VIF for the title-map wrong-prim bug. Scan the SOURCE qwords of
            // each UNPACK for triangle-class GIFtag templates (PRE=1, prim type in {3 tri,4 tristrip,
            // 5 trifan}) and print the prim the EE packet carries BEFORE unpack. Compare to the
            // post-unpack VU template / output prim (trifan 0x5D): source already trifan => EE build
            // bug; source tristrip/tri but output trifan => VIF unpack mangles it. Env DC2_G62_TPLSRC.
            static const bool s_g62tplsrc = (std::getenv("DC2_G62_TPLSRC") != nullptr);
            if (s_g62tplsrc)
            {
                static std::atomic<uint32_t> s_g62{0};
                const uint32_t scanQw = (writeVectorCount < 16u) ? writeVectorCount : 16u;
                for (uint32_t q = 0u; q < scanQw && pos + (q + 1u) * 16u <= sizeBytes; ++q)
                {
                    uint64_t tagLo = 0u, tagHi = 0u;
                    std::memcpy(&tagLo, data + pos + q * 16u, 8);
                    std::memcpy(&tagHi, data + pos + q * 16u + 8u, 8);
                    const bool pre = ((tagLo >> 46) & 1u) != 0u;
                    const uint32_t prim = static_cast<uint32_t>((tagLo >> 47) & 0x7FFu);
                    const uint32_t ptype = prim & 7u;
                    if (pre && (ptype == 3u || ptype == 4u || ptype == 5u) &&
                        s_g62.fetch_add(1u, std::memory_order_relaxed) < 256u)
                    {
                        std::fprintf(stderr,
                                     "[G62:tplsrc] dest=0x%x op=0x%02x num=%u +qw%u prim=0x%x type=%u nloop=%u tagLo=0x%016llx tagHi=0x%016llx\n",
                                     vuAddr, static_cast<uint32_t>(opcode), writeVectorCount, q,
                                     prim, ptype, static_cast<uint32_t>(tagLo & 0x7FFFu),
                                     static_cast<unsigned long long>(tagLo),
                                     static_cast<unsigned long long>(tagHi));
                    }
                }
            }
            // G39: for the skinned-model bone-matrix palette unpack (absolute dest qw 0x3c, large
            // num) dump the SOURCE bytes as floats. Distinguishes a zero EE source (game did not
            // compute the bone matrices headless) from a runtime unpack/cycle drop.
            if (vu1_trace_enabled() && vuAddr == 0x3cu && writeVectorCount >= 20u)
            {
                static std::atomic<uint32_t> s_g39pal{0};
                if (s_g39pal.fetch_add(1u, std::memory_order_relaxed) < 3u)
                {
                    std::fprintf(stderr, "[G39:palsrc] dest=0x%x num=%u cl=%u wl=%u rel=%u bytes=%u\n",
                                 vuAddr, writeVectorCount, cl, wl, (imm & 0x8000u) ? 1u : 0u, totalBytes);
                    for (uint32_t q = 0u; q < 8u && pos + (q + 1u) * 16u <= sizeBytes; ++q)
                    {
                        float f[4] = {0, 0, 0, 0};
                        std::memcpy(f, data + pos + q * 16u, 16);
                        std::fprintf(stderr, "[G39:palsrc] +qw%u = % .4g % .4g % .4g % .4g\n",
                                     q, f[0], f[1], f[2], f[3]);
                    }
                }
            }

            // G38: trace UNPACK destination vs the double-buffer TOPS so the write/read buffer
            // interleaving across multiple MSCAL kicks is visible (find why later kicks read 0).
            if (vu1_trace_enabled())
            {
                static std::atomic<uint32_t> s_g38unpk{0};
                if (s_g38unpk.fetch_add(1u, std::memory_order_relaxed) < 60u)
                    std::fprintf(stderr, "[G38:unpk] dest=0x%x imm=0x%x rel=%u tops=0x%x num=%u op=0x%02x bytes=%u\n",
                                 vuAddr, imm & 0xFFFFu, (imm & 0x8000u) ? 1u : 0u,
                                 vif1_regs.tops & 0x3FFu, writeVectorCount, (uint32_t)opcode, totalBytes);
                // G39: log every unpack during the costume model draw (armed at first model kick)
                // so the bone-matrix palette upload (dest qw ~0x3c) and the per-mesh vertex unpacks
                // are visible. Flags whether the palette is ever written in the runner stream.
                const bool armedLog = s_g39UnpkLog.load(std::memory_order_relaxed) > 0u;
                // Always catch a substantial unpack into the palette/work region (qw 0x38..0x180),
                // even if it happens once at model-load before any model kick (before arming).
                static std::atomic<uint32_t> s_g39PalLog{0};
                const bool palLog = (vuAddr >= 0x38u && vuAddr <= 0x180u && writeVectorCount >= 4u &&
                                     s_g39PalLog.fetch_add(1u, std::memory_order_relaxed) < 200u);
                if (armedLog || palLog)
                {
                    if (armedLog) s_g39UnpkLog.fetch_sub(1u, std::memory_order_relaxed);
                    std::fprintf(stderr, "[G39:unpk]%s dest=0x%x rel=%u tops=0x%x num=%u cl=%u wl=%u op=0x%02x bytes=%u\n",
                                 palLog ? "PAL" : "", vuAddr, (imm & 0x8000u) ? 1u : 0u, vif1_regs.tops & 0x3FFu,
                                 writeVectorCount, cl, wl, (uint32_t)opcode, totalBytes);
                }
            }

            // G29: catch the UNPACK that fills the low matrix/constants region (qw0..0xc) where
            // the poisoned ~-FLT_MAX projection terms live. Dump the SOURCE as floats to tell a
            // bad-source (game/EE) from a bad-decode (runtime unpack).
            if (vu1_trace_enabled() && vuAddr <= 0x0Cu)
            {
                static std::atomic<uint32_t> s_g29unpk{0};
                if (s_g29unpk.fetch_add(1u, std::memory_order_relaxed) < 6u)
                {
                    std::fprintf(stderr, "[G29:matunpk] dest=0x%x imm=0x%x op=0x%02x num=%u cl=%u wl=%u flg=%u bytes=%u\n",
                                 vuAddr, imm, (uint32_t)opcode, writeVectorCount, cl, wl,
                                 (imm & 0x8000u) ? 1u : 0u, totalBytes);
                    const uint32_t nshow = (totalBytes / 4u) < 28u ? (totalBytes / 4u) : 28u;
                    for (uint32_t w = 0u; w < nshow && pos + (w + 1u) * 4u <= sizeBytes; w += 4u)
                    {
                        float f[4] = {0,0,0,0};
                        for (uint32_t k = 0; k < 4u && pos + (w + k + 1u) * 4u <= sizeBytes; ++k)
                            std::memcpy(&f[k], data + pos + (w + k) * 4u, 4);
                        std::fprintf(stderr, "[G29:matsrc] +%u = % .4g % .4g % .4g % .4g\n",
                                     w, f[0], f[1], f[2], f[3]);
                    }
                    // G30: dump the guest render_info global (base 0x380ec0): focal[0] + the
                    // projection input matrix at +0x10..0x4f that CreateRenderInfoPacket feeds
                    // into mgMulMatrix to produce VU qw0x4..0x7. If THIS is already -FLT_MAX the
                    // bug is the projection setup (SetRenderInfo); if it is sane the bug is in the
                    // packet-build mgMulMatrix (COP2 class). HW ref: focal=800, rows
                    // (800,0,0,0)/(-61.4,-994.3,50.08,-0.03)/(-2047,..)/(2.05e5,..,4.99e7,100).
                    if (m_rdram)
                    {
                        const uint32_t riBase = 0x00380ec0u & 0x1FFFFFFFu;
                        float focal = 0.0f; std::memcpy(&focal, m_rdram + riBase, 4);
                        std::fprintf(stderr, "[G30:ri] focal=% .6g\n", focal);
                        for (uint32_t r = 0u; r < 4u; ++r)
                        {
                            float m[4] = {0,0,0,0};
                            std::memcpy(m, m_rdram + riBase + 0x10u + r * 16u, sizeof(m));
                            std::fprintf(stderr, "[G30:ri] +0x%02x = % .6g % .6g % .6g % .6g\n",
                                         0x10u + r * 16u, m[0], m[1], m[2], m[3]);
                        }
                    }
                }
            }

            // G70: find the EE source of VU1 data qword 59 (VF29 = the per-vertex W/ADC cull
            // consts). The title reads qword 59 = 0 -> degenerate cull -> flat blue (see
            // ps2_vu1.cpp [G70:qw]). Log any UNPACK whose dest range covers qword 59 (or the
            // const qwords 21/22/30) with its EE source address + the source floats for slot 59,
            // so the same EE bytes can be read on PCSX2 (HW) for an A/B, and so we can see
            // whether qword 59 is uploaded at all on the title. Env DC2_G70; quiet by default.
            {
                static const bool s_g70vif = (std::getenv("DC2_G70") != nullptr);
                if (s_g70vif)
                {
                    const uint32_t lastDest = vuAddr + ((writeVectorCount > 0u) ? (writeVectorCount - 1u) : 0u);
                    const bool covers59 = (vuAddr <= 59u && lastDest >= 59u);
                    const bool nearConst = (vuAddr <= 60u && lastDest >= 21u);
                    if (covers59 || nearConst)
                    {
                        static std::atomic<uint32_t> s_g70vn{0};
                        if (s_g70vn.fetch_add(1u, std::memory_order_relaxed) < 120u)
                        {
                            const uint32_t eeSrc = m_rdram ? (uint32_t)(((data + pos) - m_rdram) & 0x1FFFFFFFu) : 0u;
                            float s59[4] = {0, 0, 0, 0};
                            if (covers59 && m_rdram && bytesPerVector == 16u)
                            {
                                const uint32_t srcOff = pos + (59u - vuAddr) * 16u;
                                for (uint32_t k = 0; k < 4u && srcOff + (k + 1u) * 4u <= sizeBytes; ++k)
                                    std::memcpy(&s59[k], data + srcOff + k * 4u, 4);
                            }
                            std::fprintf(stderr,
                                         "[G70:unpk] dest=0x%x..0x%x covers59=%u abs=%u eeSrc=0x%x num=%u cl=%u wl=%u bpv=%u tops=0x%x src59=% .6g % .6g % .6g % .6g\n",
                                         vuAddr, lastDest, covers59 ? 1u : 0u, (imm & 0x8000u) ? 1u : 0u, eeSrc,
                                         writeVectorCount, cl, wl, bytesPerVector, vif1_regs.tops & 0x3FFu,
                                         s59[0], s59[1], s59[2], s59[3]);
                        }
                    }
                }
            }

            const bool zeroExtend = (imm & 0x4000u) != 0u;
            const bool g137Active = g136Active && g137_trace_enabled();
            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    uint32_t srcVecIndex = 0xFFFFFFFFu;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVecIndex = srcIndex;
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        g297NoteVuWrite(false, destOff, srcVec, copyBytes); // G301 MTVU ordered event
                        if (g137Active)
                        {
                            uint32_t rawCopy[4] = {};
                            std::memcpy(rawCopy, m_vu1Data + destOff, sizeof(rawCopy));
                            g137_record_origin(g136Seq, destVec, pos - 4u,
                                                srcVec ? static_cast<uint32_t>(pos + (srcVec - srcBase)) : 0xFFFFFFFFu,
                                                static_cast<uint32_t>(opcode), imm, writeIndex, srcVecIndex,
                                                cl, wl, vif1_regs.mode & 3u,
                                                vif1_regs.tops & 0x3FFu, vif1_regs.top & 0x3FFu,
                                                rawCopy, srcVec, bytesPerVector);
                        }
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                    g297NoteVuWrite(false, destOff, reinterpret_cast<const uint8_t *>(lanes),
                                    sizeof(lanes)); // G301 MTVU ordered event
                    if (g137Active)
                    {
                        g137_record_origin(g136Seq, destVec, pos - 4u,
                                            srcVec ? static_cast<uint32_t>(pos + (srcVec - srcBase)) : 0xFFFFFFFFu,
                                            static_cast<uint32_t>(opcode), imm, writeIndex, srcVecIndex,
                                            cl, wl, vif1_regs.mode & 3u,
                                            vif1_regs.tops & 0x3FFu, vif1_regs.top & 0x3FFu,
                                            lanes, srcVec, bytesPerVector);
                    }
                }
            }

            // G70: title flat-blue ROOT FIX (experimental, gated DC2_G70_FOG). The title's
            // per-vertex FOG const VF29 = VU data qword 59 is uploaded ZERO on the runner (the
            // headless title fog setup never runs SetFogParam, so the mgFlushRenderInfo source
            // globals 0x381e9c/a0/a4/a8 stay 0). With VF29=0 the VU's word3 packing saturates ->
            // the GS reads ADC=1 on ~73% of title verts (HW 32%) -> the whole rock cavern culls
            // -> flat blue. HW's qword 59 (read live from PCSX2 via those globals) =
            // (-191.25, 255, 0, 535500). Inject it when an UNPACK covering qword 59 carries an
            // all-zero source, to measure whether correct fog restores the title geometry.
            if (m_vu1Data)
            {
                static const bool s_g70fog = (std::getenv("DC2_G70_FOG") != nullptr);
                if (s_g70fog)
                {
                    const uint32_t lastDest70 = vuAddr + ((writeVectorCount > 0u) ? (writeVectorCount - 1u) : 0u);
                    if (vuAddr <= 59u && lastDest70 >= 59u && 59u * 16u + 16u <= PS2_VU1_DATA_SIZE)
                    {
                        float cur[4] = {0, 0, 0, 0};
                        std::memcpy(cur, m_vu1Data + 59u * 16u, 16);
                        if (cur[0] == 0.0f && cur[1] == 0.0f && cur[2] == 0.0f && cur[3] == 0.0f)
                        {
                            const float hwFog[4] = {-191.25f, 255.0f, 0.0f, 535500.0f};
                            std::memcpy(m_vu1Data + 59u * 16u, hwFog, 16);
                            g297NoteVuWrite(false, 59u * 16u, reinterpret_cast<const uint8_t *>(hwFog),
                                            16); // G301 MTVU ordered event
                        }
                    }
                }
            }
            // G39: dump VU memory at the palette base (qw 0x3c) right after the bone-palette
            // unpack writes, to confirm whether the data actually landed in VU data RAM.
            if (vu1_trace_enabled() && vuAddr == 0x3cu && writeVectorCount >= 20u && m_vu1Data)
            {
                static std::atomic<uint32_t> s_g39paldst{0};
                if (s_g39paldst.fetch_add(1u, std::memory_order_relaxed) < 3u)
                {
                    for (uint32_t q = 0x3cu; q < 0x44u; ++q)
                    {
                        float f[4] = {0, 0, 0, 0};
                        std::memcpy(f, m_vu1Data + q * 16u, 16);
                        std::fprintf(stderr, "[G39:paldst] vu qw0x%x = % .4g % .4g % .4g % .4g\n",
                                     q, f[0], f[1], f[2], f[3]);
                    }
                }
            }
            pos += totalBytes;

            if (g22KickMode == G22Vu1KickMode::Unpack &&
                (g22IsV432 || (g22IsLargeUnpack && sizeBytes <= 0x10000u)))
            {
                g22KickResidentVu1("unpack", g17LastOpcodePos, opcode, writeVectorCount, vuAddr,
                                    g22_vu1_start_pc_override(kDc2ResidentVu1Pc));
            }

            if (vu1_trace_enabled() && g17FirstMscntOff != 0xFFFFFFFFu &&
                g17LastOpcodePos < g17FirstMscntOff && g17FirstMscntOff < pos)
            {
                static std::atomic<uint32_t> s_g17unp{0};
                if (s_g17unp.fetch_add(1u, std::memory_order_relaxed) < 32u)
                    std::fprintf(stderr,
                                 "[G17:swallow] UNPACK cmdPos=0x%x op=0x%02x imm=0x%04x num=%u bytes=%u span=[0x%x,0x%x) mscntOff=0x%x\n",
                                 g17LastOpcodePos, static_cast<uint32_t>(opcode), imm,
                                 static_cast<uint32_t>(num), totalBytes, g17LastOpcodePos, pos,
                                 g17FirstMscntOff);
            }

            if (pos > sizeBytes)
            {
                g17BreakReason = "unpack-overrun";
                break;
            }
            continue;
        }
        else
        {
            continue;
        }
    }

    if (g22KickMode == G22Vu1KickMode::Stream ||
        g22KickMode == G22Vu1KickMode::Resume ||
        g22KickMode == G22Vu1KickMode::AllStreams)
    {
        const bool deformLikeStream =
            sizeBytes >= 512u &&
            sizeBytes <= 0x10000u &&
            g22DirectCommands > 0u &&
            (g22V432Unpacks > 0u || g22LargeUnpacks > 0u);

        if (g22KickMode == G22Vu1KickMode::AllStreams || deformLikeStream)
        {
            g22KickResidentVu1((g22KickMode == G22Vu1KickMode::AllStreams) ? "all-stream" : "stream",
                               0xFFFFFFFFu, 0u, 0u, 0u,
                               g22_vu1_start_pc_override(kDc2ResidentVu1Pc));
        }
    }

    if (g136Active)
    {
        std::fprintf(stderr,
                     "[G136:vifend] seq=%u stopPos=0x%x size=%u reason=%s direct=%u v432=%u large=%u mpg320=%u loggedCmds=%u loggedPayloads=%u\n",
                     g136Seq, pos, sizeBytes, g17BreakReason, g22DirectCommands,
                     g22V432Unpacks, g22LargeUnpacks, g22Mpg320Uploads,
                     g136LoggedCommands, g136LoggedPayloads);
    }

    if (vu1_trace_enabled())
    {
        // Log only the streams big enough to plausibly carry a model chain, when the parser
        // stopped well short of the end (early break / desync) — i.e. a swallowed kick.
        static std::atomic<uint32_t> s_g17end{0};
        if (sizeBytes > 0x10000u && pos + 64u < sizeBytes)
        {
            const uint32_t e = s_g17end.fetch_add(1u, std::memory_order_relaxed);
            if (e < 32u)
                std::fprintf(stderr,
                             "[G17:parseend] e=%u stopPos=0x%x size=%u reason=%s lastOp=0x%02x lastOpPos=0x%x\n",
                             e, pos, sizeBytes, g17BreakReason,
                             static_cast<uint32_t>(g17LastOpcode), g17LastOpcodePos);
        }
    }

    if (vu1_trace_enabled())
    {
        static std::atomic<uint32_t> s_sum{0};
        const uint32_t n = s_sum.fetch_add(1u, std::memory_order_relaxed);
        if (n < 64u || (n % 256u) == 0u)
            std::fprintf(stderr, "[F51.7:vifsum] call=%u ops=%u mpg=%u unpack=%u direct=%u mscal=%u mscnt=%u rawMscalWords=%u rawMscntWords=%u\n",
                         n,
                         s_f517Ops.load(std::memory_order_relaxed),
                         s_f517Mpg.load(std::memory_order_relaxed),
                         s_f517Unpack.load(std::memory_order_relaxed),
                         s_f517Direct.load(std::memory_order_relaxed),
                         s_f517Mscal.load(std::memory_order_relaxed),
                         s_f517Mscnt.load(std::memory_order_relaxed),
                         s_f517RawMscalBytes.load(std::memory_order_relaxed),
                         s_f517RawMscntBytes.load(std::memory_order_relaxed));
        std::fprintf(stderr, "[G17:v432] count=%u\n", s_f517V432Unpack.load(std::memory_order_relaxed));
    }

    g95DrainDeferredGif("vif-end");
}
