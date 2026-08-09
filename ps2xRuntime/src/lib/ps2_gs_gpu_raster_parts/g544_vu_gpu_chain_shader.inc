// G544: derive the chained executor from G543's already-proven semantic source. The only replaced
// pieces are XGKICK publication and main(): every opcode/pipeline/float helper stays token-identical.
static bool g544ReplaceShaderPart(std::string &source, const std::string &from,
                                  const std::string &to, std::string &why)
{
    const size_t at = source.find(from);
    if (at == std::string::npos)
    {
        why = "G543 shader anchor not found";
        return false;
    }
    source.replace(at, from.size(), to);
    return true;
}

static std::string g544ChainShaderSource(std::string &why)
{
    std::string source(g543SemanticShaderSource());
    const std::string globals =
        "uvec4 Rram[1024];\n"
        "uvec4 Rcode[1024];\n"
        "uint Rpipe[161];\n"
        "uint gRingPacketCapacity;\n"
        "uint gRingByteCapacity;\n"
        "uint gRingConsumerPackets;\n"
        "uint gRingConsumerBytes;\n"
        "const uint F_CHAIN = 0x00000080u;\n"
        "const uint F_RING = 0x00000100u;\n";
    if (!g544ReplaceShaderPart(source, "uvec4 Rram[1024];\n", globals, why))
        return {};

    const size_t kickBegin = source.find("void emitKick(int isReg, uint instr)");
    const size_t stageP = source.find("void stageP", kickBegin);
    const size_t kickEnd = stageP == std::string::npos
        ? std::string::npos
        : source.rfind("\n}", stageP);
    if (kickBegin == std::string::npos || stageP == std::string::npos ||
        kickEnd == std::string::npos || kickEnd < kickBegin)
    {
        why = "G543 emitKick body not found";
        return {};
    }
    static const char kRingKick[] = R"G544K(void emitKick(int isReg, uint instr)
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
        if (flg == 3u) {
            fail(F_RING, instr);
            return;
        }
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
    if (!done) {
        fail(F_RING, instr);
        return;
    }
    if (total == 0u) return;
    uint outstandingPackets = gPacketCount - gRingConsumerPackets;
    uint outstandingBytes = gPacketBytes - gRingConsumerBytes;
    if (gRingPacketCapacity == 0u || gRingByteCapacity == 0u ||
        outstandingPackets >= gRingPacketCapacity || total > gRingByteCapacity ||
        outstandingBytes > gRingByteCapacity - total) {
        fail(F_RING, instr);
        return;
    }
    uint slot = gPacketCount % gRingPacketCapacity;
    storeOutU32(gPacketDescByte + slot * 8u, addr);
    storeOutU32(gPacketDescByte + slot * 8u + 4u, total);
    for (uint b = 0u; b < total; ++b) {
        uint byteSlot = (gPacketBytes + b) % gRingByteCapacity;
        storeOutByte(gPacketBytesByte + byteSlot, ramByte(addr + b));
    }
    ++gPacketCount;
    gPacketBytes += total;
}
)G544K";
    source.replace(kickBegin, kickEnd + 2u - kickBegin, kRingKick);

    const size_t mainAt = source.rfind("void main()\n{");
    if (mainAt == std::string::npos)
    {
        why = "G543 main body not found";
        return {};
    }
    source.resize(mainAt);
    source += R"G544A(uint payloadByte(uint byteAddr)
{
    uint word = pw(byteAddr >> 2u);
    return (word >> ((byteAddr & 3u) * 8u)) & 255u;
}

void storeRamByte(uint byteAddr, uint value)
{
    uint a = byteAddr & 0x3fffu;
    uint q = a >> 4u;
    int lane = int((a >> 2u) & 3u);
    uint shift = (a & 3u) * 8u;
    uint mask = 255u << shift;
    Rram[q][lane] = (Rram[q][lane] & ~mask) | ((value & 255u) << shift);
}

void storeCodeByte(uint byteAddr, uint value)
{
    uint a = byteAddr & 0x3fffu;
    uint q = a >> 4u;
    int lane = int((a >> 2u) & 3u);
    uint shift = (a & 3u) * 8u;
    uint mask = 255u << shift;
    Rcode[q][lane] = (Rcode[q][lane] & ~mask) | ((value & 255u) << shift);
}

uint chainCodeWord(uint byteAddr)
{
    uint a = byteAddr & 0x3fffu;
    return Rcode[a >> 4u][int((a >> 2u) & 3u)];
}

uint currentStateWord(uint word)
{
    if (word < 128u) return Rvf[word >> 2u][int(word & 3u)];
    if (word < 144u) return uint(Rvi[word - 128u]);
    if (word < 148u) return Racc[int(word - 144u)];
    if (word == 148u) return Rq;
    if (word == 149u) return Rp;
    if (word == 150u) return Ri;
    if (word == 151u) return Rpc;
    if (word == 152u) return Rmac;
    if (word == 153u) return Rclip;
    if (word == 154u) return Rstatus;
    if (word == 155u) return Rebit;
    if (word == 156u) return Ritop;
    return Rxitop;
}

void syncPipeArray()
{
    Rpipe[2] = Qpending; Rpipe[3] = uint(Qdelay);
    Rpipe[4] = Ppending; Rpipe[5] = uint(Pdelay);
    Rpipe[22] = MacVisible; Rpipe[23] = StatusVisible; Rpipe[73] = G428Claim;
    for (int s = 0; s < 8; ++s) {
        for (int n = 0; n < 4; ++n) Rpipe[82 + s*4+n] = Hvec[s][n];
        Rpipe[114 + s*2] = Hstamp[s].x; Rpipe[115 + s*2] = Hstamp[s].y;
        Rpipe[130 + s] = Hmac[s]; Rpipe[138 + s] = Hdest[s];
        Rpipe[146 + s] = Hkind[s];
    }
    Rpipe[154] = uint(Hhead); Rpipe[155] = PairBase.x; Rpipe[156] = PairBase.y;
    Rpipe[157] = HistArmed ? 1u : 0u;
}

void loadChainInitial()
{
    loadRecord();
    uint prePipe = cw(gCmd + 17u) >> 2u;
    for (uint n = 0u; n < 161u; ++n) Rpipe[n] = pw(prePipe + n);
    uint code = cw(gCmd + 19u) >> 2u;
    for (int q = 0; q < 1024; ++q)
        Rcode[q] = uvec4(pw(code + uint(q*4+0)), pw(code + uint(q*4+1)),
                             pw(code + uint(q*4+2)), pw(code + uint(q*4+3)));
}

void applyIncomingWrites()
{
    uint desc = cw(gCmd + 22u) >> 2u;
    uint bytesBase = cw(gCmd + 24u);
    uint cursor = 0u;
    for (uint w = 0u; w < cw(gCmd + 23u); ++w) {
        uint off = pw(desc + w*3u + 0u);
        uint len = pw(desc + w*3u + 1u);
        bool code = pw(desc + w*3u + 2u) != 0u;
        if (cursor > cw(gCmd + 25u) || len > cw(gCmd + 25u) - cursor) {
            fail(F_CHAIN, 0x57524954u);
            return;
        }
        for (uint b = 0u; b < len; ++b) {
            uint value = payloadByte(bytesBase + cursor + b);
            if (code) storeCodeByte(off + b, value); else storeRamByte(off + b, value);
        }
        cursor += len;
    }
    if (cursor != cw(gCmd + 25u)) fail(F_CHAIN, 0x57425954u);
}

void verifyChainPre()
{
    uint preState = cw(gCmd + 15u) >> 2u;
    for (uint w = 0u; w < 158u; ++w) {
        if (currentStateWord(w) != pw(preState + w)) { fail(F_CHAIN, 0x53000000u | w); return; }
    }
    uint prePipe = cw(gCmd + 17u) >> 2u;
    for (uint w = 0u; w < 161u; ++w) {
        if (Rpipe[w] != pw(prePipe + w)) { fail(F_CHAIN, 0x50000000u | w); return; }
    }
    uint code = cw(gCmd + 19u) >> 2u;
    uint data = cw(gCmd + 20u) >> 2u;
    for (int q = 0; q < 1024; ++q) {
        for (int n = 0; n < 4; ++n) {
            uint w = uint(q*4+n);
            if (Rcode[q][n] != pw(code + w)) { fail(F_CHAIN, 0x43000000u | w); return; }
            if (Rram[q][n] != pw(data + w)) { fail(F_CHAIN, 0x44000000u | w); return; }
        }
    }
}

void setupRecord()
{
    gPostStateByte = cw(gCmd + 16u); gPostPipeByte = cw(gCmd + 18u);
    gCodeByte = cw(gCmd + 19u); gPreDataByte = cw(gCmd + 20u); gPostDataByte = cw(gCmd + 21u);
    gTop = cw(gCmd + 8u);
    if (cw(gCmd) != ABI_MAGIC || cw(gCmd + 1u) != 1u || cw(gCmd + 2u) != 32u ||
        gPostStateByte - cw(gCmd + 15u) != 632u || cw(gCmd + 17u) - gPostStateByte != 632u ||
        gPostPipeByte - cw(gCmd + 17u) != 644u || gCodeByte - gPostPipeByte != 644u ||
        gPreDataByte - gCodeByte != 16384u || gPostDataByte - gPreDataByte != 16384u)
        gErrors |= F_LAYOUT;
}
)G544A";
    source += R"G544B(
void executeChainRecord(out uint cycle, out uint stop)
{
    bool resume = (cw(gCmd + 3u) & 1u) != 0u;
    Rebit = 0u;
    Ritop = cw(gCmd + 7u);
    if (!resume) {
        Rpc = cw(gCmd + 6u);
        Rvf[0] = uvec4(0u, 0u, 0u, 0x3f800000u);
    }

    uint budget = cw(gCmd + 9u);
    cycle = 0u;
    stop = 0u;
    uint pendingBranch = 0xffffffffu;
    for (; cycle < budget; ++cycle) {
        uint currentPc = Rpc;
        if (currentPc + 8u > 16384u) { stop = 1u; break; }
        uint lower = chainCodeWord(currentPc);
        uint upper = chainCodeWord(currentPc + 4u);
        bool eBit = ((upper >> 30u) & 1u) != 0u;
        bool iBit = ((upper >> 31u) & 1u) != 0u;

        if (Qdelay > 0 && --Qdelay == 0) Rq = Qpending;
        if (Pdelay > 0 && --Pdelay == 0) Rp = Ppending;
        if (!iBit && ((lower >> 25u) & 0x7fu) == 0x40u) {
            uint fn = s2func(lower);
            bool qProducer = fn >= 0x38u && fn <= 0x3au;
            if (Qdelay > 0 && (fn == 0x3bu || qProducer)) { Rq = Qpending; Qdelay = 0; }
            bool pProducer = fn == 0x70u || fn == 0x71u || fn == 0x72u || fn == 0x73u ||
                             fn == 0x76u || fn == 0x78u || fn == 0x79u || fn == 0x7au;
            if (Pdelay > 0 && (fn == 0x7bu || pProducer)) { Rp = Ppending; Pdelay = 0; }
        }

        uint lowerOp = (lower >> 25u) & 0x7fu;
        if (!iBit && lowerReadsStatus(lowerOp)) fail(F_PIPE, lower);
        if (!iBit && lowerReadsMac(lowerOp)) MacVisible = resolveMac(add64s(add64u(PairBase, cycle), -4));

        uvec2 stamp = add64u(PairBase, cycle);
        if (iBit) {
            Ri = lower;
            bool upperVnop = (upper & 63u) >= 0x3cu && s2func(upper) == 0x2fu;
            if (!upperVnop) {
                Rmac = resolveMac(stamp);
                execUpper(upper, true, stamp);
                G428Claim = 2u;
                if (gErrors == 0u) pushEager(stamp, Rmac);
            }
        } else {
            int hazard = upperVfDest(upper);
            uvec4 oldV = uvec4(0u), newV = uvec4(0u);
            if (hazard >= 0) oldV = Rvf[hazard];
            execUpper(upper, false, stamp);
            if (hazard >= 0) { newV = Rvf[hazard]; Rvf[hazard] = oldV; }
            if (gErrors == 0u) execLower(lower);
            if (hazard >= 0) applyBits(Rvf[hazard], newV, (upper >> 21u) & 15u);
        }
        if (gErrors != 0u) { stop = 3u; break; }

        Rvf[0] = uvec4(0u, 0u, 0u, 0x3f800000u);
        Rvi[0] = 0;
        uint postExecPc = Rpc;
        uint sequential = currentPc + 8u;
        if (sequential >= 16384u) sequential = 0u;
        uint nextPc = sequential;
        if (pendingBranch != 0xffffffffu) {
            nextPc = pendingBranch;
            pendingBranch = 0xffffffffu;
        } else if (postExecPc != currentPc) {
            pendingBranch = postExecPc + 8u;
            if (pendingBranch >= 16384u) pendingBranch = 0u;
        }
        if (nextPc >= 16384u) nextPc = 0u;
        Rpc = nextPc;
        if (Rebit != 0u) { stop = 2u; break; }
        if (eBit) Rebit = 1u;
    }

    if (Pdelay > 0 && Rebit != 0u) { Rp = Ppending; Pdelay = 0; }
    Rmac = resolveMac(add64u(PairBase, cycle));
    PairBase = add64u(PairBase, cycle);
}

void writeRecordStatus(uint cycle, uint stop, uint packetDelta, uint byteDelta)
{
    uint statusAt = cw(gCmd + 31u);
    sw(statusAt + 0u, 0x34344753u); // little-endian "SG44"
    sw(statusAt + 1u, gErrors);
    sw(statusAt + 2u, gFailPc);
    sw(statusAt + 3u, gFailWord);
    sw(statusAt + 4u, cycle);
    sw(statusAt + 5u, stop);
    sw(statusAt + 6u, packetDelta);
    sw(statusAt + 7u, byteDelta);
}

void storePersistentWorkspace(uint base)
{
    for (uint w = 0u; w < 158u; ++w) sw(base + w, currentStateWord(w));
    base += 158u;
    for (uint w = 0u; w < 161u; ++w) sw(base + w, Rpipe[w]);
    base += 161u;
    for (int q = 0; q < 1024; ++q)
        for (int n = 0; n < 4; ++n) sw(base + uint(q*4+n), Rcode[q][n]);
    base += 4096u;
    for (int q = 0; q < 1024; ++q)
        for (int n = 0; n < 4; ++n) sw(base + uint(q*4+n), Rram[q][n]);
}
)G544B";
    source += R"G544C(
void main()
{
    if (gl_GlobalInvocationID.x != 0u) return;
    uint records = cw(30u);
    uint ringDescByte = cw(32u + 30u);
    uint ringBytesByte = cw(64u + 30u);
    gRingPacketCapacity = cw(96u + 30u);
    gRingByteCapacity = cw(128u + 30u);
    uint metaWord = cw(160u + 30u);
    uint workspaceWord = cw(192u + 30u);
    if (records < 7u || gRingPacketCapacity == 0u || gRingByteCapacity == 0u) return;

    gPacketDescByte = ringDescByte;
    gPacketBytesByte = ringBytesByte;
    gPacketCount = 0u; gPacketBytes = 0u;
    gRingConsumerPackets = 0u; gRingConsumerBytes = 0u;
    Rpc = 0xffffffffu;

    for (uint rec = 0u; rec < records; ++rec) {
        gCmd = rec * 32u;
        gErrors = 0u; gFailPc = 0xffffffffu; gFailWord = 0u;
        setupRecord();
        if (rec == 0u) {
            loadChainInitial();
            armHistory();
            // armHistory() writes G428's transition through pipeOut(), because G543 owns only an
            // output checkpoint. G544 also owns persistent pipe state: absorb that complete
            // checkpoint before the first execution so record 1 sees every transitioned word.
            uint armedPipe = gPostPipeByte >> 2u;
            for (uint w = 0u; w < 161u; ++w) Rpipe[w] = ow(armedPipe + w);
        } else {
            applyIncomingWrites();
            if (gErrors == 0u) verifyChainPre();
        }
        if (gErrors != 0u) {
            writeRecordStatus(0u, 3u, 0u, 0u);
            return;
        }

        uint packetStart = gPacketCount;
        uint bytesStart = gPacketBytes;
        uint cycle = 0u, stop = 0u;
        executeChainRecord(cycle, stop);
        uint packetDelta = gPacketCount - packetStart;
        uint byteDelta = gPacketBytes - bytesStart;
        if (cycle != cw(gCmd + 10u) || cw(gCmd + 11u) != 0u || stop != cw(gCmd + 12u))
            gErrors |= F_RESULT;
        if (packetDelta != cw(gCmd + 27u) || byteDelta != cw(gCmd + 29u))
            gErrors |= F_PACKET;

        syncPipeArray();
        uint postPipe = gPostPipeByte >> 2u;
        for (uint w = 0u; w < 161u; ++w) sw(postPipe + w, Rpipe[w]);
        storeRecord();
        writeRecordStatus(cycle, stop, packetDelta, byteDelta);
        if (gErrors != 0u) return;
    }

    sw(metaWord + 0u, 0x41523434u); // little-endian "44RA"
    sw(metaWord + 1u, 1u);
    sw(metaWord + 2u, records);
    sw(metaWord + 3u, gRingPacketCapacity);
    sw(metaWord + 4u, gRingByteCapacity);
    sw(metaWord + 5u, gPacketCount);
    sw(metaWord + 6u, gPacketBytes);
    sw(metaWord + 7u, gRingConsumerPackets);
    sw(metaWord + 8u, gRingConsumerBytes);
    sw(metaWord + 9u, 0u);
    sw(metaWord + 10u, workspaceWord);
    sw(metaWord + 11u, 8511u);
    storePersistentWorkspace(workspaceWord);
}
)G544C";
    return source;
}
