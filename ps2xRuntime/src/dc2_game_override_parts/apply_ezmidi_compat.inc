void applyDC2EzMidiCompat(PS2Runtime &runtime)
{
    PS2EzMidiCompatLayout layout{};
    layout.rpcSid = 0x00012346u;
    layout.reportedFreeIopBytes = 0x00200000u;
    layout.defaultPortVolume = 0x00000100u;
    layout.reportedVoiceCount = 0x00000020u;
    runtime.iop().setEzMidiCompatLayout(layout);
}
