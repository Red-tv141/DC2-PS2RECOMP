// G545: derive the live shadow from G544's proven chained semantic source. Opcode, pipeline, and
// float helpers remain identical. Only packet publication and the transport/workspace main change.
static std::string g545LiveShaderSource(std::string &why)
{
    std::string source = g544ChainShaderSource(why);
    if (source.empty())
        return {};

    const std::string extraGlobals =
        "const uint F_RING = 0x00000100u;\n"
        "const uint F_LIVE_ABI = 0x00000200u;\n"
        "const uint F_LIVE_SEQUENCE = 0x00000400u;\n"
        "const uint F_LIVE_STATE = 0x00000800u;\n"
        "const uint F_LIVE_DATA = 0x00001000u;\n"
        "uniform int uG545CommandCount;\n";
    if (!g544ReplaceShaderPart(source, "const uint F_RING = 0x00000100u;\n",
                               extraGlobals, why))
        return {};

    const size_t kickBegin = source.find("void emitKick(int isReg, uint instr)");
    const size_t stageP = source.find("void stageP", kickBegin);
    const size_t kickEnd = stageP == std::string::npos
        ? std::string::npos : source.rfind("\n}", stageP);
    if (kickBegin == std::string::npos || stageP == std::string::npos ||
        kickEnd == std::string::npos || kickEnd < kickBegin)
    {
        why = "G544 emitKick body not found";
        return {};
    }
    static const char kCompareKick[] = R"G545K(void emitKick(int isReg, uint instr)
{
    uint addr = (u16(Rvi[isReg]) * 16u) & 0x3fffu;
    uint off = addr;
    uint total = 0u;
    bool done = false;
    for (int safety = 0; safety < 256 && !done; ++safety) {
        uint lo = readRamU32(off);
        uint hi = readRamU32(off + 4u);
        uint nloop = lo & 0x7fffu;
        uint flg = (hi >> 26u) & 3u;
        uint nreg = (hi >> 28u) & 15u;
        if (nreg == 0u) nreg = 16u;
        if (flg == 3u) { fail(F_RING, instr); return; }
        uint size = 16u;
        if (flg == 0u) size += nloop * nreg * 16u;
        else if (flg == 1u) {
            uint regs = nloop * nreg;
            size += regs * 8u + ((regs & 1u) != 0u ? 8u : 0u);
        } else if (flg == 2u) size += nloop * 16u;
        total += size;
        off = (off + size) & 0x3fffu;
        done = ((lo >> 15u) & 1u) != 0u;
    }
    if (!done || gPacketCount >= gExpectedPackets ||
        total > gExpectedPacketBytes - min(gPacketBytes, gExpectedPacketBytes)) {
        fail(F_PACKET | F_RING, instr);
        return;
    }
    uint desc = gPacketDescByte + gPacketCount * 8u;
    uint expectedAddr = pw(desc >> 2u);
    uint expectedBytes = pw((desc >> 2u) + 1u);
    if (addr != expectedAddr || total != expectedBytes) {
        fail(F_PACKET, 0x4b440000u | (gPacketCount & 0xffffu));
        return;
    }
    for (uint b = 0u; b < total; ++b) {
        uint expectedWord = pw((gPacketBytesByte + gPacketBytes + b) >> 2u);
        uint expected = (expectedWord >> (((gPacketBytesByte + gPacketBytes + b) & 3u) * 8u)) & 255u;
        uint actual = ramByte(addr + b);
        if (actual != expected) {
            // Failure detail: [31:24]=GPU byte, [23:16]=CPU byte, [15:0]=packet offset.
            // One word is enough to distinguish arithmetic drift from address/ordering drift
            // without adding a synchronous diagnostic readback to the live path.
            fail(F_PACKET, (actual << 24u) | (expected << 16u) | (b & 0xffffu));
            return;
        }
    }
    ++gPacketCount;
    gPacketBytes += total;
}
)G545K";
    source.replace(kickBegin, kickEnd + 2u - kickBegin, kCompareKick);

    const size_t mainAt = source.rfind("void main()\n{");
    if (mainAt == std::string::npos)
    {
        why = "G544 main body not found";
        return {};
    }
    source.resize(mainAt);
    source += R"G545A(
const uint LIVE_MAGIC = 0x35343547u; // little-endian "G545"
const uint LIVE_VERSION = 1u;
const uint LIVE_STATE_WORD = 16u;
const uint LIVE_PIPE_WORD = 174u;
const uint LIVE_CODE_WORD = 335u;
const uint LIVE_DATA_WORD = 4431u;

void loadLivePrivatePipe()
{
    Qpending = Rpipe[2]; Qdelay = int(Rpipe[3]);
    Ppending = Rpipe[4]; Pdelay = int(Rpipe[5]);
    MacVisible = Rpipe[22]; StatusVisible = Rpipe[23]; G428Claim = Rpipe[73];
    for (int s = 0; s < 8; ++s) {
        Hvec[s] = uvec4(Rpipe[82+s*4+0], Rpipe[82+s*4+1],
                        Rpipe[82+s*4+2], Rpipe[82+s*4+3]);
        Hstamp[s] = uvec2(Rpipe[114+s*2], Rpipe[115+s*2]);
        Hmac[s] = Rpipe[130+s]; Hdest[s] = Rpipe[138+s]; Hkind[s] = Rpipe[146+s];
    }
    Hhead = int(Rpipe[154]); PairBase = uvec2(Rpipe[155], Rpipe[156]);
    HistArmed = Rpipe[157] != 0u;
}

void loadLiveWorkspace()
{
    uint base = LIVE_STATE_WORD;
    for (int r = 0; r < 32; ++r)
        Rvf[r] = uvec4(ow(base+uint(r*4+0)), ow(base+uint(r*4+1)),
                       ow(base+uint(r*4+2)), ow(base+uint(r*4+3)));
    for (int r = 0; r < 16; ++r) Rvi[r] = int(ow(base + 128u + uint(r)));
    Racc = uvec4(ow(base+144u), ow(base+145u), ow(base+146u), ow(base+147u));
    Rq=ow(base+148u); Rp=ow(base+149u); Ri=ow(base+150u); Rpc=ow(base+151u);
    Rmac=ow(base+152u); Rclip=ow(base+153u); Rstatus=ow(base+154u);
    Rebit=ow(base+155u); Ritop=ow(base+156u); Rxitop=ow(base+157u);
    for (uint w=0u; w<161u; ++w) Rpipe[w]=ow(LIVE_PIPE_WORD+w);
    loadLivePrivatePipe();
    for (int q=0; q<1024; ++q) {
        Rcode[q]=uvec4(ow(LIVE_CODE_WORD+uint(q*4+0)), ow(LIVE_CODE_WORD+uint(q*4+1)),
                       ow(LIVE_CODE_WORD+uint(q*4+2)), ow(LIVE_CODE_WORD+uint(q*4+3)));
        Rram[q]=uvec4(ow(LIVE_DATA_WORD+uint(q*4+0)), ow(LIVE_DATA_WORD+uint(q*4+1)),
                      ow(LIVE_DATA_WORD+uint(q*4+2)), ow(LIVE_DATA_WORD+uint(q*4+3)));
    }
}

void publishLiveStatus(uint errors, uvec2 failSeq, uint failRec, uvec2 total,
                       uvec2 lastSeq, uint dispatches, uint packets,
                       uint checkpoints, uint seeds)
{
    sw(0u, LIVE_MAGIC); sw(1u, LIVE_VERSION); sw(2u, errors);
    sw(3u, failSeq.x); sw(4u, failSeq.y); sw(5u, failRec);
    sw(6u, gFailPc); sw(7u, gFailWord);
    sw(8u, total.x); sw(9u, total.y); sw(10u, lastSeq.x); sw(11u, lastSeq.y);
    sw(12u, dispatches); sw(13u, packets); sw(14u, checkpoints); sw(15u, seeds);
}

bool verifyLivePost(bool checkpoint)
{
    uint state = cw(gCmd + 16u) >> 2u;
    for (uint w=0u; w<158u; ++w) {
        if (currentStateWord(w) != pw(state+w)) {
            fail(F_LIVE_STATE, 0x53000000u | w); return false;
        }
    }
    uint pipe = cw(gCmd + 18u) >> 2u;
    for (uint w=0u; w<161u; ++w) {
        if (Rpipe[w] != pw(pipe+w)) {
            fail(F_PIPE, 0x50000000u | w); return false;
        }
    }
    if (checkpoint) {
        uint data = cw(gCmd + 21u) >> 2u;
        for (int q=0; q<1024; ++q) for (int n=0; n<4; ++n) {
            uint w=uint(q*4+n);
            if (Rram[q][n] != pw(data+w)) {
                fail(F_LIVE_DATA, 0x44000000u | w); return false;
            }
        }
    }
    return true;
}
)G545A";
    source += R"G545B(
void main()
{
    if (gl_GlobalInvocationID.x != 0u) return;
    uint commandCount = uint(max(uG545CommandCount, 0));
    if (commandCount == 0u) return;

    bool initialized = ow(0u) == LIVE_MAGIC && ow(1u) == LIVE_VERSION;
    if (initialized && ow(2u) != 0u) return;
    uvec2 total = initialized ? uvec2(ow(8u), ow(9u)) : uvec2(0u);
    uvec2 lastSeq = initialized ? uvec2(ow(10u), ow(11u)) : uvec2(0u);
    uint dispatches = initialized ? ow(12u) + 1u : 1u;
    uint packets = initialized ? ow(13u) : 0u;
    uint checkpoints = initialized ? ow(14u) : 0u;
    uint seeds = initialized ? ow(15u) : 0u;
    if (initialized) loadLiveWorkspace();

    for (uint rec=0u; rec<commandCount; ++rec) {
        gCmd = rec * 32u;
        gErrors=0u; gFailPc=0xffffffffu; gFailWord=0u;
        gPacketCount=0u; gPacketBytes=0u;
        Rpc=initialized ? Rpc : 0xffffffffu;
        uint flags=cw(gCmd+3u);
        bool seed=(flags & 2u) != 0u;
        bool checkpoint=(flags & 4u) != 0u;
        uvec2 seq=uvec2(cw(gCmd+4u), cw(gCmd+5u));
        if (cw(gCmd+0u) != 0x35435556u || cw(gCmd+1u) != 1u ||
            cw(gCmd+2u) != 32u || cw(gCmd+14u) != cw(gCmd+31u))
            fail(F_LIVE_ABI, 0x41424935u);
        if (!seed) {
            if (!initialized) fail(F_LIVE_SEQUENCE, 0x53454544u);
            else {
                uvec2 expected=add64u(lastSeq, 1u);
                if (seq != expected) fail(F_LIVE_SEQUENCE, 0x53455135u);
            }
        }

        gPostStateByte=cw(gCmd+16u);
        gPostPipeByte=LIVE_PIPE_WORD*4u;
        gCodeByte=cw(gCmd+19u); gPreDataByte=cw(gCmd+20u); gPostDataByte=cw(gCmd+21u);
        gPacketDescByte=cw(gCmd+26u); gExpectedPackets=cw(gCmd+27u);
        gPacketBytesByte=cw(gCmd+28u); gExpectedPacketBytes=cw(gCmd+29u);
        gTop=cw(gCmd+8u);

        if (gErrors == 0u && seed) {
            loadChainInitial();
            armHistory();
            for (uint w=0u; w<161u; ++w) Rpipe[w]=ow(LIVE_PIPE_WORD+w);
            initialized=true;
            ++seeds;
        } else if (gErrors == 0u) {
            applyIncomingWrites();
        }
        if (gErrors != 0u) {
            publishLiveStatus(gErrors, seq, rec, total, lastSeq, dispatches,
                              packets, checkpoints, seeds);
            return;
        }

        uint cycle=0u, stop=0u;
        executeChainRecord(cycle, stop);
        if (cycle != cw(gCmd+10u) || cw(gCmd+11u) != 0u || stop != cw(gCmd+12u))
            gErrors |= F_RESULT;
        if (gPacketCount != gExpectedPackets || gPacketBytes != gExpectedPacketBytes)
            gErrors |= F_PACKET;
        syncPipeArray();
        if (gErrors == 0u) verifyLivePost(checkpoint);
        if (gErrors != 0u) {
            publishLiveStatus(gErrors, seq, rec, total, lastSeq, dispatches,
                              packets, checkpoints, seeds);
            return;
        }

        total=add64u(total, 1u); lastSeq=seq; packets += gPacketCount;
        if (checkpoint) ++checkpoints;
    }

    storePersistentWorkspace(LIVE_STATE_WORD);
    publishLiveStatus(0u, lastSeq, 0xffffffffu, total, lastSeq, dispatches,
                      packets, checkpoints, seeds);
}
)G545B";
    return source;
}
