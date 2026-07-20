void applyDC2Phase9Stubs(PS2Runtime &runtime)
{
    using namespace ps2_game_overrides;

    g_g7_poll_live_pad_hook = &g7_poll_live_pad;
    g_g55_title_draw_probe_hook = &g55_title_draw_probe;
    g_f66_drive_dungeon_pad_hook = &f66_drive_dungeon_pad;

    // -------------------------------------------------------------------------
    // PHASE9: DC2 ג€” Direct function overrides (not in PS2_STUB_LIST)
    // -------------------------------------------------------------------------
    // Placement operator new/new[] must return $a1 (the pointer), not -1.
    runtime.registerFunction(0x001398E0u, f50_1_placement_new_probe); // __nw__FUiP1 (reta1 + F50.1 ctor-id probe)
    runtime.registerFunction(0x001398F0u, reta1_stub); // __nwa__FUiP1

    // 0x29fc78: unrecompiled function in TitleInit range, called by printfג†’Alloc chain.
    // Without this, the runtime enters a crash loop on missing-function recovery.
    runtime.registerFunction(0x0029FC78u, nop_stub);

    // -------------------------------------------------------------------------
    // Group D ג€” Debug console (sceDevFont / sceDevCons): return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00104288u, "memclr");
    bindAddressHandler(runtime, 0x00104D58u, "sceDevFontDefault");
    bindAddressHandler(runtime, 0x00104DE0u, "sceDevFontIdle");
    bindAddressHandler(runtime, 0x00104E20u, "sceDevFontSetColor");
    bindAddressHandler(runtime, 0x00104E58u, "sceDevConsInit");
    bindAddressHandler(runtime, 0x00104E90u, "sceDevConsOpen");
    bindAddressHandler(runtime, 0x00104F90u, "sceDevConsClose");
    bindAddressHandler(runtime, 0x00104FC8u, "sceDevConsRef");
    bindAddressHandler(runtime, 0x00105068u, "sceDevConsDraw");
    bindAddressHandler(runtime, 0x00105230u, "sceDevConsDrawS");
    bindAddressHandler(runtime, 0x001053E0u, "sceDevConsClear");
    bindAddressHandler(runtime, 0x00105438u, "sceDevConsSetColor");
    bindAddressHandler(runtime, 0x00105450u, "sceDevConsPrintf");
    bindAddressHandler(runtime, 0x001056A8u, "sceDevConsLocate");
    bindAddressHandler(runtime, 0x001056D8u, "sceDevConsPut");
    bindAddressHandler(runtime, 0x001057A8u, "sceDevConsGet");
    bindAddressHandler(runtime, 0x00105820u, "sceDevConsAttribute");
    bindAddressHandler(runtime, 0x00105828u, "sceDevConsClearBox");
    bindAddressHandler(runtime, 0x001058F0u, "sceDevConsMove");
    bindAddressHandler(runtime, 0x00105A58u, "sceDevConsRollup");
    bindAddressHandler(runtime, 0x00105B08u, "sceDevConsMessage");
    bindAddressHandler(runtime, 0x00105C28u, "sceDevConsFrame");
    bindAddressHandler(runtime, 0x00105E18u, "sceDevFontRefDirectImage");
    bindAddressHandler(runtime, 0x00105FA0u, "sceDevFontRefStrN");
    bindAddressHandler(runtime, 0x00106100u, "sceDevConsPutc");
    bindAddressHandler(runtime, 0x00106150u, "sceDevConsGetc");
    bindAddressHandler(runtime, 0x001061A0u, "sceDevConsPiece");
    bindAddressHandler(runtime, 0x00106208u, "sceDevFontKnj2Chr");
    bindAddressHandler(runtime, 0x001064F0u, "sceDevFont");

    // -------------------------------------------------------------------------
    // Group D ג€” GIF packet builders (sceGifPk*): return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00106630u, "sceGifPkInit");
    bindAddressHandler(runtime, 0x00106640u, "sceGifPkReset");
    bindAddressHandler(runtime, 0x00106650u, "sceGifPkTerminate");
    bindAddressHandler(runtime, 0x001066A8u, "sceGifPkCnt");
    bindAddressHandler(runtime, 0x00106720u, "sceGifPkRef");
    bindAddressHandler(runtime, 0x001067B8u, "sceGifPkEnd");
    bindAddressHandler(runtime, 0x00106830u, "sceGifPkReserve");
    bindAddressHandler(runtime, 0x00106848u, "sceGifPkOpenGifTag");
    bindAddressHandler(runtime, 0x00106860u, "sceGifPkCloseGifTag");
    bindAddressHandler(runtime, 0x00106908u, "sceGifPkAddGsData");
    bindAddressHandler(runtime, 0x00106930u, "sceGifPkAddGsAD");
    bindAddressHandler(runtime, 0x00106958u, "sceGifPkRefLoadImage");

    // -------------------------------------------------------------------------
    // Group D ג€” VIF1 packet builders (sceVif1Pk*): return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00106B18u, "sceVif1PkInit");
    bindAddressHandler(runtime, 0x00106B28u, "sceVif1PkReset");
    bindAddressHandler(runtime, 0x00106B38u, "sceVif1PkTerminate");
    bindAddressHandler(runtime, 0x00106B90u, "sceVif1PkCnt");
    bindAddressHandler(runtime, 0x00106BE8u, "sceVif1PkCall");
    bindAddressHandler(runtime, 0x00106C58u, "sceVif1PkEnd");
    bindAddressHandler(runtime, 0x00106CB0u, "sceVif1PkOpenDirectCode");
    bindAddressHandler(runtime, 0x00106D08u, "sceVif1PkCloseDirectCode");
    bindAddressHandler(runtime, 0x00106D38u, "sceVif1PkOpenGifTag");
    bindAddressHandler(runtime, 0x00106D50u, "sceVif1PkCloseGifTag");
    bindAddressHandler(runtime, 0x00106DF8u, "sceVif1PkReserve");
    bindAddressHandler(runtime, 0x00106E10u, "sceVif1PkAlign");
    bindAddressHandler(runtime, 0x00106E90u, "sceVif1PkAddGsAD");

    // -------------------------------------------------------------------------
    // Group D ג€” Misc internal helpers
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x0010C6A8u, "dmaRefImage");
    bindAddressHandler(runtime, 0x0010DA28u, "_system_header");
    bindAddressHandler(runtime, 0x0010FE70u, "ReleaseAlarm");
    bindAddressHandler(runtime, 0x00110330u, "EnableCache");
    bindAddressHandler(runtime, 0x00110340u, "DisableCache");
    bindAddressHandler(runtime, 0x001104B0u, "isceSifSetDma");
    bindAddressHandler(runtime, 0x001104D0u, "isceSifSetDChain");
    bindAddressHandler(runtime, 0x00110550u, "_InitTLB");
    bindAddressHandler(runtime, 0x00110E48u, "InitAlarm");
    bindAddressHandler(runtime, 0x00111B30u, "printfloat");

    // -------------------------------------------------------------------------
    // Group D ג€” Filesystem internals (_sceFs*): return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x001139A0u, "_sceFsIobSemaMK");
    bindAddressHandler(runtime, 0x00113AF8u, "_sceFs_Rcv_Intr");
    bindAddressHandler(runtime, 0x00113F08u, "_sceFsWaitS");
    bindAddressHandler(runtime, 0x00113F48u, "scePowerOffHandler");
    bindAddressHandler(runtime, 0x00113FD8u, "_sceFs_Poff_Intr");

    // -------------------------------------------------------------------------
    // Group D ג€” Extended filesystem / I/O ops: return 0 is correct for PC
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00115158u, "sceIoctl2");
    bindAddressHandler(runtime, 0x00115338u, "_sceCallCode");
    bindAddressHandler(runtime, 0x001154E0u, "sceRemove");
    bindAddressHandler(runtime, 0x00115500u, "sceMkdir");
    bindAddressHandler(runtime, 0x001156B0u, "sceRmdir");
    bindAddressHandler(runtime, 0x001156D0u, "sceFormat");
    bindAddressHandler(runtime, 0x00115940u, "sceAddDrv");
    bindAddressHandler(runtime, 0x00115A60u, "sceDelDrv");
    bindAddressHandler(runtime, 0x00115A80u, "sceDopen");
    bindAddressHandler(runtime, 0x00115B48u, "sceDclose");
    bindAddressHandler(runtime, 0x00115CB0u, "sceDread");
    bindAddressHandler(runtime, 0x00115E08u, "sceGetstat");
    bindAddressHandler(runtime, 0x00115FA8u, "sceChstat");
    bindAddressHandler(runtime, 0x001161E8u, "sceRename");
    bindAddressHandler(runtime, 0x001163D8u, "sceChdir");
    bindAddressHandler(runtime, 0x001163F8u, "sceSync");
    bindAddressHandler(runtime, 0x00116590u, "sceMount");
    bindAddressHandler(runtime, 0x00116800u, "sceUmount");
    bindAddressHandler(runtime, 0x00116820u, "sceLseek64");
    // sceDevctl is only reached from HddConectCheck/CheckInstallSpace; the name-based bind
    // cannot resolve it (not in PS2_STUB_LIST), which left the recompiled TODO wrapper live
    // and fatally throwing "Unimplemented PS2 stub called: sceDevctl". Register the real
    // no-HDD stub directly so the HDD probe fails cleanly (-11 -> retail no-HDD path).
    runtime.registerFunction(0x00116A58u, &ps2_stubs::sceDevctl);
    bindAddressHandler(runtime, 0x00116C90u, "sceSymlink");
    bindAddressHandler(runtime, 0x00116E70u, "sceReadlink");

    // -------------------------------------------------------------------------
    // Group D ג€” SIF IOP memory / module management: return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00117130u, "sceSifAllocSysMemory");
    bindAddressHandler(runtime, 0x001171B0u, "sceSifFreeSysMemory");
    bindAddressHandler(runtime, 0x00117910u, "sceSifUnloadModule");
    bindAddressHandler(runtime, 0x001179A0u, "sceSifSearchModuleByName");
    bindAddressHandler(runtime, 0x00117A40u, "sceSifSearchModuleByAddress");
    bindAddressHandler(runtime, 0x00117AF0u, "sceSifLoadStartModuleBuffer");
    bindAddressHandler(runtime, 0x00117D58u, "sceSifLoadStartModule");

    // -------------------------------------------------------------------------
    // Group D ג€” TLB management: no-op for recompiled code
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00118570u, "_SetTLBEntry");
    bindAddressHandler(runtime, 0x00118580u, "SetTLBEntry");
    bindAddressHandler(runtime, 0x001185C0u, "GetTLBEntry");
    bindAddressHandler(runtime, 0x00118610u, "InitTLB");

    // -------------------------------------------------------------------------
    // Group D ג€” CD internal callbacks: return 0 is safe
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x0011F500u, "_sceCd_cd_callback");
    bindAddressHandler(runtime, 0x0011F738u, "_sceCd_cd_read_intr");
    bindAddressHandler(runtime, 0x0011F8F0u, "sceCdPOffCallback");
    bindAddressHandler(runtime, 0x0011F960u, "_sceCd_Poff_Intr");
    bindAddressHandler(runtime, 0x0011FD08u, "_sceCd_ncmd_prechk");
    bindAddressHandler(runtime, 0x00120020u, "_sceCd_scmd_prechk");

    // -------------------------------------------------------------------------
    // Group D ג€” Memory card internals: return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00122560u, "sceMcEnd");
    bindAddressHandler(runtime, 0x00122D78u, "mcHearAlarm");
    bindAddressHandler(runtime, 0x00122DA0u, "mcDelayThread");
    bindAddressHandler(runtime, 0x001230A8u, "sceMcUdCheckNewCard");

    // -------------------------------------------------------------------------
    // Group D ג€” MIDI/sound input: return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00123C10u, "sceMSIn_Init");
    bindAddressHandler(runtime, 0x00123C90u, "sceMSIn_ATick");
    bindAddressHandler(runtime, 0x00123C98u, "sceMSIn_Load");
    bindAddressHandler(runtime, 0x00123D38u, "sceMSIn_PutMsg");
    bindAddressHandler(runtime, 0x00123DE8u, "sceMSIn_PutExcMsg");
    bindAddressHandler(runtime, 0x00123E60u, "sceMSIn_PutHsMsg");

    // -------------------------------------------------------------------------
    // Group B ג€” Memory/libc helpers (real implementations in dc2_auto_stubs.cpp)
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00123EA8u, "abort");
    bindAddressHandler(runtime, 0x001264D8u, "malloc_extend_top");
    bindAddressHandler(runtime, 0x001272D8u, "__malloc_lock");
    bindAddressHandler(runtime, 0x001272E0u, "__malloc_unlock");
    bindAddressHandler(runtime, 0x00127C48u, "__mcmp");
    bindAddressHandler(runtime, 0x0012A990u, "__sprint");
    bindAddressHandler(runtime, 0x0012A9D8u, "__sbprintf");

    // -------------------------------------------------------------------------
    // Group A ג€” Math functions (real implementations in dc2_auto_stubs.cpp)
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x0012A6A8u, "strtod");
    bindAddressHandler(runtime, 0x0012A6D8u, "strtodf");
    bindAddressHandler(runtime, 0x0012A700u, "_strtol_r");
    bindAddressHandler(runtime, 0x0012A938u, "strtol");
    bindAddressHandler(runtime, 0x0012A970u, "toupper");
    bindAddressHandler(runtime, 0x0012C1D0u, "exponent");
    bindAddressHandler(runtime, 0x001297A8u, "_strtod_r");
    bindAddressHandler(runtime, 0x00123EE0u, "atof");
    bindAddressHandler(runtime, 0x00123F00u, "atoi");
    bindAddressHandler(runtime, 0x0011E2B8u, "atanf");
    bindAddressHandler(runtime, 0x0011E590u, "cosf");
    bindAddressHandler(runtime, 0x0011E678u, "fabsf");
    bindAddressHandler(runtime, 0x0011E698u, "floorf");
    bindAddressHandler(runtime, 0x0011E908u, "sinf");
    bindAddressHandler(runtime, 0x0011E9F8u, "tanf");
    bindAddressHandler(runtime, 0x0011F1D8u, "atan2f");
    bindAddressHandler(runtime, 0x0011F300u, "sqrtf");
    bindAddressHandler(runtime, 0x00286078u, "__divdi3");

    // -------------------------------------------------------------------------
    // Group C ג€” Game-specific (real implementations in dc2_auto_stubs.cpp)
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00118E50u, "InitExecPS2");
    bindAddressHandler(runtime, 0x00149240u, "GetFullPath__FPcPc");
    // strFile* (0x0029AB10..0x0029AE80) removed ג€” Phase B: let recompiled strFile
    // code run so it can reach sceCdSearchFile (now ISO-backed).
    // init__Fv must stay bound: it is a TODO stub (not recompiled) and must return 0.
    bindAddressHandler(runtime, 0x0015C160u, "init__Fv");
    bindAddressHandler(runtime, 0x0014A3D0u, "pad_button_read__FP10PAD_STATUSii");
    bindAddressHandler(runtime, 0x00186BD0u, "divby0error__Fv");
    bindAddressHandler(runtime, 0x001CBD70u, "memoryInit__Fv");
    bindAddressHandler(runtime, 0x0029B4E0u, "iopGetArea__FPiPiPiPiP8AudioDeci");
    bindAddressHandler(runtime, 0x002F4BA0u, "McError__18CMemoryCardManagerFi");
    bindAddressHandler(runtime, 0x002F5390u, "McCheckMCPs2__FP12MC_CARD_INFO");
    bindAddressHandler(runtime, 0x002F53D0u, "McCheckMCPs2Boot__FP12MC_CARD_INFOi");
    bindAddressHandler(runtime, 0x0030A290u, "SCElogoFade__FiP9mgCMemory");
    bindAddressHandler(runtime, 0x0031CE80u, "random__Fv");
    bindAddressHandler(runtime, 0x0031D760u, "abs__Ff");
    bindAddressHandler(runtime, 0x00320680u, "rand_prob__Fi");

    // _ZERO_VECTOR appears at two distinct guest addresses, same handler
    bindAddressHandler(runtime, 0x002767B0u, "_ZERO_VECTOR__FP12RS_STACKDATAi");
    bindAddressHandler(runtime, 0x002E3390u, "_ZERO_VECTOR__FP12RS_STACKDATAi");

    // Pad auto-repeat helpers
    bindAddressHandler(runtime, 0x00278620u, "_PAD_AUTO_REPEAT_OFF__FP12RS_STACKDATAi");
    bindAddressHandler(runtime, 0x00278650u, "_PAD_SET_AUTO_REPEAT__FP12RS_STACKDATAi");

    // 64-bit integer division (GCC runtime helper)
    // __divdi3 already bound above in Group A

    // HDD/partition helpers: no HDD on PC, return 0
    bindAddressHandler(runtime, 0x0031B940u, "CreateDir__FPc");
    bindAddressHandler(runtime, 0x0031BAC0u, "CheckPartition__Fv");
    bindAddressHandler(runtime, 0x0031BBC0u, "CheckAppInstall__Fv");
    bindAddressHandler(runtime, 0x0031BD50u, "MountHDDFileSystem__Fv");
    bindAddressHandler(runtime, 0x0031BE30u, "UmountHDDFileSystem__Fv");
    bindAddressHandler(runtime, 0x0031BE60u, "CheckInstallSpace__Fv");
    bindAddressHandler(runtime, 0x0031BF60u, "UninstallApp__Fv");

    // -------------------------------------------------------------------------
    // PHASE F3 ג€” Three critical auto-stubs on the SCE-logo + main-menu code path.
    // These OVERRIDE the bindAddressHandler entries above (last write wins in the
    // function map). See plans/phase-F2-diagnosis.md and plans/phase-F3-fix-log.md.
    // -------------------------------------------------------------------------
    runtime.registerFunction(0x0015C160u, f55_boot_init_stub);       // init__Fv (nop'd, but F55 runs the real InitCDFile@0x148F70)
    runtime.registerFunction(0x0030A290u, scelogofade_done_stub);    // SCElogoFade
    runtime.registerFunction(0x0014A490u, read_pad_stub);            // read_pad
    runtime.registerFunction(0x0014A3D0u, pad_button_read_stub);     // pad_button_read
    // G7-fix: SIF IOP module-management funcs whose bindAddressHandler entries do
    // not resolve to a ps2_stubs impl (so the throwing recompiled TODO body stays
    // live). Override with a return-0 stub so probing for an IOP module no longer
    // ends the process (the IOP/module subsystem is faked in this port).
    runtime.registerFunction(0x00117910u, sif_module_return_zero_stub); // sceSifUnloadModule
    runtime.registerFunction(0x001179A0u, sif_module_return_zero_stub); // sceSifSearchModuleByName
    runtime.registerFunction(0x00117A40u, sif_module_return_zero_stub); // sceSifSearchModuleByAddress
    runtime.registerFunction(0x00117AF0u, sif_module_return_zero_stub); // sceSifLoadStartModuleBuffer
    runtime.registerFunction(0x00117D58u, sif_module_return_zero_stub); // sceSifLoadStartModule
    // PHASE F41: title_mode_key_stub retired. Real TitleModeKey__Fv at 0x002A1220
    // now runs via register_functions.cpp. F40 pad injection drives confirm via R1.
    runtime.registerFunction(0x001B0780u, g224_getpoly_probe);       // G224: camera-poly query result (DngMainKey camera block reach + fail signal)
    runtime.registerFunction(0x002EC110u, g224_camctl_step_probe);   // G224: scene camera Step (nextPos->pos commit) reach probe
    runtime.registerFunction(0x00131990u, g224_setfollow_probe);     // G224: follow-target value probe
    runtime.registerFunction(0x001CF090u, g224_dngmaindraw_probe);   // G224: DngMainDraw entry reach probe
    runtime.registerFunction(0x001648F0u, g129_cfg_water_vertex_probe); // G129: SPI WATER_VERTEX map-config handler
    runtime.registerFunction(0x0029FFA0u, f29_title_loop_probe);     // TitleLoop
    runtime.registerFunction(0x002A1020u, g94_title_mode_init_probe); // G94: state-1 init postcondition (projection/camera)
    runtime.registerFunction(0x002A1220u, f49_title_mode_key_probe); // TitleModeKey timing/path probe
    runtime.registerFunction(0x002A2AD0u, f49_title_mccheck_key_probe); // TitleMCCheckKey timing/path probe
    runtime.registerFunction(0x002F1FC0u, f49_mc_manager_step_probe); // CMemoryCardManager::Step timing/path probe
    runtime.registerFunction(0x0023E1B0u, f49_menu_check_push_probe); // MenuCheckPushButton path probe
    runtime.registerFunction(0x0023E2E0u, f49_convert_check_push_probe); // ConvertCheckPushButton path probe
    runtime.registerFunction(0x00232DF0u, f49_menu_main_init_probe); // MenuMainInit timing/path probe
    runtime.registerFunction(0x00233FF0u, f49_menu_main_key_probe); // MenuMainKey timing/path probe
    runtime.registerFunction(0x00251100u, g11_loadfilemenu_probe); // G11 costume sheet load
    runtime.registerFunction(0x0012DA90u, g11_enterimg_probe);     // G11 IMG -> texture-mgr entry
    runtime.registerFunction(0x0012E850u, g11_reloadtex_probe);    // G11 ReloadTexture tbp bind
    runtime.registerFunction(0x002F19A0u, f49_finish_for_mc_probe); // FinishForMC timing/path probe
    runtime.registerFunction(0x001908A0u, f49_init_save_data_probe); // InitSaveData timing/path probe
    runtime.registerFunction(0x00196BE0u, f49_get_user_data_man_probe); // GetUserDataMan path probe
    runtime.registerFunction(0x00195D40u, f50_7_get_item_file_path_probe); // F50.7: fallback cfg7 costume model paths for debug dungeon route
    runtime.registerFunction(0x0019D560u, f49_set_chr_equip_probe); // SetChrEquipDirect path probe
    runtime.registerFunction(0x001A1B80u, f49_debug_get_item_probe); // DebugGetItem path probe
    runtime.registerFunction(0x0019B490u, f49_get_chara_data_ptr_probe); // GetCharaDataPtr path probe
    runtime.registerFunction(0x002AFDB0u, f49_set_menu_load_item_no_probe); // SetMenuLoadItemNo path probe
    runtime.registerFunction(0x002B90D0u, f49_menu_item_chara_data_load_probe); // MenuItemCharaDataLoad path probe
    runtime.registerFunction(0x00148E70u, f49_read_bg_sync_probe); // ReadBGSync polling probe
    runtime.registerFunction(0x002BBC80u, f48_2_read_main_chara_bg_probe); // ReadMainCharaBG step-23 audit probe
    runtime.registerFunction(0x002BC150u, f48_2_key_main_chara_bg_probe); // KeyMainCharaBG step-23 audit probe
    runtime.registerFunction(0x002BE030u, f48_2_menu_costume_key_probe); // MenuCostumeKey step-23 audit probe
    runtime.registerFunction(0x001CBD70u, f50_1_memory_init);             // F50.1 memoryInit (was auto-stub) ג€” create dungeon mgCMemory pools
    runtime.registerFunction(0x0012E600u, f50_9_mg_load_image_probe);     // F50.9 mgLoadImage CLUT/texture upload probe (DC2_TRACE_F50_9)
    runtime.registerFunction(0x0012E850u, f51_reload_texture_packet_probe); // F51 ReloadTexture packet-wrapper reach probe (DC2_TRACE_T8_UPLOAD)
    runtime.registerFunction(0x0012E970u, f50_10_reload_texture_probe);   // F50.10 ReloadTexture post-reload TEX0 vs data/CLUT ptr probe (DC2_TRACE_F50_10)
    runtime.registerFunction(0x00134DA0u, f50_11_drawprim_texture_probe); // F50.11 mgCDrawPrim::Texture bind-source probe (DC2_TRACE_F50_11)
    runtime.registerFunction(0x0012D050u, f50_11_get_texture_probe);      // F50.11 GetTexture owner/name probe (DC2_TRACE_F50_11)
    // F50.2: sndLoadSound stub REVERTED ג€” stubbing it broke the boot sound-init
    // vsync handshake (hung earlier at mgGetVSyncCount@0x141310). Real body needed.
    // runtime.registerFunction(0x0018DA30u, f50_2_snd_load_sound_stub);
    runtime.registerFunction(0x001CC040u, f48_4_init_dungeon_main_probe); // F48.4 InitDungeonMain post-costume new-game loop init probe
    runtime.registerFunction(0x0029FF30u, f48_4_title_exit_probe);        // F49.5 TitleExit (LoopExit[3]) loop-transition bracket
    runtime.registerFunction(0x0014B600u, f48_4_capture_end_probe);       // F49.5 CaptureEnd loop-transition bracket (last call before transition sceGsSyncV)
    runtime.registerFunction(0x001CEA00u, f48_4_loop_dungeon_main_probe); // F48.4 LoopDungeonMain post-costume new-game loop frame probe
    runtime.registerFunction(0x001EFF40u, f56_treemap_step_probe);        // F56 floor-select treemap state machine probe (DC2_TRACE_F56)
    runtime.registerFunction(0x002A0AB0u, f29_title_draw_probe);     // TitleDraw
    runtime.registerFunction(0x002A1B60u, f29_title_mode_draw_probe);// TitleModeDraw
    // PHASE F38: TitleMapDraw bypass removed ג€” real recomp registered by
    // recomp/register_functions.cpp is left in effect now that F33 fixed the
    // mgCDrawPrim ctor so draw primitives initialize correctly.
    // runtime.registerFunction(0x002A2280u, title_map_draw_stub);      // TitleMapDraw
    runtime.registerFunction(0x002A2280u, f48_title_map_draw_probe);  // TitleMapDraw camera-vtable fixup + delegate
    runtime.registerFunction(0x002838C0u, g58_getcamera_title_fix);   // G58: assign title camera on demand (ordering fix; kill DC2_G58_NO_CAMFIX)
    runtime.registerFunction(0x00168FD0u, g59_get_texture_block_no_probe); // G59 title texture-block probe (DC2_TRACE_G59)
    runtime.registerFunction(0x002A2548u, g59_title_map_resume_2548); // G59 title resume repair: stack + saved return + camera s7
    runtime.registerFunction(0x002A2644u, g59_title_map_resume_2644); // G59 title resume repair: MDS reload loop continuation
    runtime.registerFunction(0x00131A20u, g59_cam_adddistance_fix);   // G59 title camera arg repair for phase-0 follow
    runtime.registerFunction(0x00131A10u, g59_cam_getdistance_fix);   // G59 title camera arg repair for phase-0 follow
    runtime.registerFunction(0x00131A50u, g59_cam_addheight_fix);     // G59 title camera arg repair for phase-0 follow
    runtime.registerFunction(0x001387F0u, g59_frame_draw_tailcall_fix); // G59 Draw__8mgCFrameFv tail-call completion
    runtime.registerFunction(0x00137E10u, g67_frame_draw_probe);      // G67 title visual 0x987230 distance-cull repair
    runtime.registerFunction(0x00135C70u, g67_frame_test1_probe);     // G67 clamp only that title frame's test1 distance result
    runtime.registerFunction(0x0012F2E0u, g67_frame_clipbox_probe);    // G67 force only that title frame's clip-box result visible
    runtime.registerFunction(0x0012F380u, g78_frame_clipinbox_fix);    // G78 force only that title frame's clip-in-box -> copy render-mode (fc4=0); kill DC2_G78_NO_COPY_FIX
    runtime.registerFunction(0x00234290u, g79_main_draw_probe);        // G79 probe: per-frame active menu id (DC2_TRACE_G79)
    runtime.registerFunction(0x002BE040u, g79_costume_draw_probe);     // G79 probe: is costume DRAW still dispatched after exit? (DC2_TRACE_G79)
    runtime.registerFunction(0x00142F90u, g59_mgdraw_frame_tailfix);  // G59 title mgDraw trace around frame-tail return
    runtime.registerFunction(0x00285670u, f48_load_map_from_memory_probe); // CScene::LoadMapFromMemory(scene,map,info)
    runtime.registerFunction(0x002856F0u, f48_load_map_from_memory2_probe); // CScene::LoadMapFromMemory(scene,map,stage,info)
    runtime.registerFunction(0x00285CE0u, f50_7_load_map_probe); // F50.7: seed missing debug-menu map stack, then CScene::LoadMap
    runtime.registerFunction(0x00164480u, f48_load_map_file_probe); // CMap::LoadMapFile
    runtime.registerFunction(0x001600D0u, f48_create_map_probe); // CMap::CreateMap
    runtime.registerFunction(0x001643A0u, f50_7_funcpoint_set_scale_probe); // F50.7: keep map-script CFuncPoint setter tailcalls resumable
    runtime.registerFunction(0x001643C0u, f50_7_funcpoint_set_rotation_probe); // F50.7: keep map-script CFuncPoint setter tailcalls resumable
    runtime.registerFunction(0x001643E0u, f50_7_funcpoint_set_position_probe); // F50.7: keep map-script CFuncPoint setter tailcalls resumable
    runtime.registerFunction(0x00143720u, f50_7_mg_active_lighting_probe); // F50.7: keep mgActiveLighting tailcall resumable
    runtime.registerFunction(0x00160B10u, f50_7_map_draw_probe); // F50.7: keep CMap::Draw tailcall resumable
    runtime.registerFunction(0x00160B30u, f50_7_map_draw_direct_probe); // F50.7: keep CMap::DrawDirect tailcall resumable
    runtime.registerFunction(0x0015E3D0u, g56_mapparts_draw_tap);   // G56 Draw__9CMapParts (part vt+0x34) chain tap
    runtime.registerFunction(0x00166A00u, g56_mapparts_predraw_tap);// G56 PreDraw__9CMapParts (part vt+0x40 gate) chain tap
    runtime.registerFunction(0x00166E40u, g56_mappiece_draw_tap);   // G56 Draw__9CMapPiece (piece vt+0x34, geometry emit) chain tap
    runtime.registerFunction(0x00135230u, g56_begindraw_tap);       // G56 BeginDraw__14mgCDrawManager mempool/alloc tap
    // G57: confirmed via temporary detector taps that Draw__12mgCVisualMDT IS drawn 6x (the
    // title model geometry submits) and DrawWater returns clean. The DrawWater skip below
    // ruled the water draw OUT as the $ra-corruption source (see g57_drawwater_skip).
    runtime.registerFunction(0x0015E800u, g57_drawwater_skip);      // G57 DrawWater skip bisection (DC2_G57_SKIP_WATER)
    runtime.registerFunction(0x002C8820u, g194_skip_draweffect);    // G194 DrawEffect skip bisection (DC2_G194_SKIP_DRAWEFFECT)
    runtime.registerFunction(0x0017E320u, g194_skip_dof);           // G194 DepthOfField skip bisection (DC2_G194_SKIP_DOF)
    runtime.registerFunction(0x0013BD90u, g194_skip_texanime);      // G194 TexAnime skip bisection (DC2_G194_SKIP_TEXANIME)
    runtime.registerFunction(0x0029C1C0u, g195_skip_editmap_draweffect); // G195 CEditMap effect skip (DC2_G195_SKIP_EDITMAP_DRAWEFFECT)
    runtime.registerFunction(0x0015E3F0u, g195_skip_cmap_draweffect);    // G195 CMap effect skip probe (DC2_G195_SKIP_CMAP_DRAWEFFECT)
    runtime.registerFunction(0x00144560u, g195_moveimage_trace);     // G195 move-image copy probe (DC2_G195_MOVEIMAGE_TRACE)
    // G58: $ra-corruption canary over the title geometry-queue chain. Registered LAST so it
    // wins the function table over the G56/F50.7 taps it wraps. Only installed under
    // DC2_TRACE_G58 so the default dispatch table (and golden title smoke) is untouched.
    if (dc2_env_flag_enabled("DC2_TRACE_G58"))
    {
        runtime.registerFunction(0x00160B10u, g58_canary_map_draw);       // Draw__4CMap (wraps f50_7 tailfix)
        runtime.registerFunction(0x001B4130u, g58_canary_editmap_drawsub);// DrawSub__8CEditMap
        runtime.registerFunction(0x0015E250u, g58_canary_map_drawsub);    // DrawSub__4CMap (loads s0=*(map+0x364))
        runtime.registerFunction(0x0015E3D0u, g58_canary_parts_draw);     // Draw__9CMapParts (wraps g56 tap)
        runtime.registerFunction(0x00166A70u, g58_canary_parts_drawsub);  // DrawSub__9CMapParts
        runtime.registerFunction(0x00166E40u, g58_canary_piece_draw);     // Draw__9CMapPiece (wraps g56 tap)
        runtime.registerFunction(0x00169FD0u, g58_canary_objframe_draw);  // Draw__12CObjectFrame
        runtime.registerFunction(0x00137E10u, g58_canary_mgframe_draw);   // Draw__8mgCFrame
        runtime.registerFunction(0x0013F4E0u, g58_canary_visualmdt_draw); // Draw__12mgCVisualMDT
        runtime.registerFunction(0x00131A20u, g58_cam_adddistance);       // AddDistance (r31/a0 probe)
        runtime.registerFunction(0x00131A10u, g58_cam_getdistance);       // GetDistance (r31/a0 probe)
        runtime.registerFunction(0x00131A50u, g58_cam_addheight);         // AddHeight (r31/a0 probe)
        runtime.registerFunction(0x00283740u, g58_assign_camera_probe);   // AssignCamera args/result probe
    }
    // G193 FIX (always registered): EditInit wrapper repairs the MainScene CScene vtable
    // (F50.4 pattern) before the scene-entry Initialize dispatch, so the town/edit route
    // gets cameras like the dungeon route does. Trace output stays gated by DC2_TRACE_G193;
    // the repair itself is default-ON (kill DC2_G193_NO_SCENEVT_FIX=1).
    runtime.registerFunction(0x001A9F40u, g193_edit_init_probe);            // EditInit (LoopInit[1] dispatch)
    if (dc2_env_flag_enabled("DC2_TRACE_G193"))
    {
        runtime.registerFunction(0x00282EA0u, g193_scene_initialize_probe); // Initialize__6CScene (virtual dispatch)
        runtime.registerFunction(0x00283740u, g193_assign_camera_probe);    // AssignCamera w/ count-at-entry
    }
    runtime.registerFunction(0x0028BE40u, f50_7_random_circle_step_probe); // F50.7: repair CRandomCircle global vtable and resume tailcall
    runtime.registerFunction(0x001EA890u, f55_calc_glid_put_pos_probe); // F55: CDngFreeMap::CalcGlidPutPos A/B input/output probe (DC2_TRACE_F55)
    runtime.registerFunction(0x001EF9F0u, f55_init_end_probe);          // F55: CMenuTreeMap::InitEnd reach probe (DC2_TRACE_F55)
    runtime.registerFunction(0x001EABE0u, f55_set_texture_info_probe);  // F55: CDngFreeMap::SetTextureInfo result probe (DC2_TRACE_F55)
    runtime.registerFunction(0x00149CD0u, f55_get_pack_file_probe);     // F55: GetPackFile name/result probe (DC2_TRACE_F55)
    runtime.registerFunction(0x00148C70u, f55_get_read_bg_file_probe);  // F55: GetReadBGFile(i) result probe (DC2_TRACE_F55)
    runtime.registerFunction(0x00148930u, f55_load_file_bg_probe);      // F55: LoadFileBG name/result probe (DC2_TRACE_F55)
    runtime.registerFunction(0x001488D0u, f55_init_read_bg_probe);      // F55: InitReadBG slot-clear probe (DC2_TRACE_F55)
    runtime.registerFunction(0x00148CC0u, f55_start_read_bg_probe);     // F55: StartReadBG slot-clear probe (DC2_TRACE_F55)
    runtime.registerFunction(0x00191970u, f50_6_menu_init_probe);     // MenuInit debug-menu probe
    runtime.registerFunction(0x00191C30u, f49_menu_loop_probe);       // MenuLoop timing probe
    runtime.registerFunction(0x00233FC0u, f49_menu_main_loop_probe);  // MenuMainLoop timing probe
    runtime.registerFunction(0x00309DE0u, f49_pause_loop_probe);      // PauseLoop timing probe
    runtime.registerFunction(0x0014A930u, f49_update_gamepad_probe);  // UpDate hotspot probe
    runtime.registerFunction(0x002ED550u, f49_update_pad_control_probe); // CPadControl::Update hotspot probe
    runtime.registerFunction(0x0018D650u, f49_snd_step_probe);        // sndStep hotspot probe
    runtime.registerFunction(0x002A7940u, f49_scene_step_snd_probe);  // CScene::StepSnd hotspot probe
    runtime.registerFunction(0x0021FE60u, f29_primquad_drawprim_probe);
    runtime.registerFunction(0x0021FF30u, f29_primquad_texture_probe);
    runtime.registerFunction(0x0012C480u, mgctexture_ct_stub);       // mgCTexture ctor
    runtime.registerFunction(0x0012C6D0u, mgctextureblock_ct_stub);  // mgCTextureBlock ctor
    runtime.registerFunction(0x0012C7E0u, mgctexturemanager_ct_stub); // mgCTextureManager ctor
    runtime.registerFunction(0x002370B0u, basemenuclass_ct_stub);    // CBaseMenuClass ctor
    runtime.registerFunction(0x002BC500u, menucostumesel_ct_stub);   // CMenuCostumeSel ctor
    runtime.registerFunction(0x00138850u, mgcdrawenv_ct_stub);       // mgCDrawEnv ctor
    runtime.registerFunction(0x001343A0u, mgcdrawprim_ct_stub);      // mgCDrawPrim ctor
    runtime.registerFunction(0x00135180u, mgcdrawmanager_ct_stub);   // mgCDrawManager ctor
    runtime.registerFunction(0x00135B60u, mgcframeattr_ct_stub);     // mgCFrameAttr ctor
    runtime.registerFunction(0x00136490u, mgcframe_ct_stub);         // mgCFrame ctor
    runtime.registerFunction(0x0013D260u, mgctextureanime_ct_stub);  // mgCTextureAnime ctor
    runtime.registerFunction(0x0017D1F0u, mgc3dsprite_ct_stub);      // mgC3DSprite ctor
    runtime.registerFunction(0x001344A0u, f29_begin_prim_probe);     // Begin mgCDrawPrim
    runtime.registerFunction(0x00134690u, f29_end_prim_probe);       // End mgCDrawPrim
    runtime.registerFunction(0x00134C80u, f33_color_probe);          // Color mgCDrawPrim
    runtime.registerFunction(0x00134DA0u, f33_texture_probe);        // Texture mgCDrawPrim
    runtime.registerFunction(0x00142560u, f51_mgenddraw_reload_probe); // F51 mgEndDrawReloadTexture reach probe (DC2_TRACE_T8_UPLOAD)
    runtime.registerFunction(0x001425B0u, f29_mgendframe_probe);     // mgEndFrame
    runtime.registerFunction(0x00141200u, f49_vsync_callback_probe); // VSyncCallBack timing/counter probe
    runtime.registerFunction(0x001412A0u, f49_waitvsync_probe);      // WaitVSync (F49 pacing probe + clamp)
    runtime.registerFunction(0x00103300u, f49_scegssyncv_probe);     // sceGsSyncV wait/callsite probe
    runtime.registerFunction(0x001041E8u, f49_syncv_callback_set_probe); // sceGsSyncVCallback set/unset probe
    // PHASE G15: deform-mesh build-path probes (only when DC2_TRACE_G15 is set, so default
    // smoke is byte-identical). Pass-through wrappers measuring mgVif1Packet growth.
    if (g15_trace_enabled())
    {
        runtime.registerFunction(0x00137E10u, g18_frame_draw_probe);  // G18: Draw__8mgCFrame (mesh build gate)
        runtime.registerFunction(0x0016B940u, g15_actor_draw_probe);  // DrawDirect__12CActionChara
        runtime.registerFunction(0x001731F0u, g15_char_draw_probe);   // DrawDirect__11CCharacter2
        runtime.registerFunction(0x00142FD0u, g15_mgdrawdirect_probe);// mgDrawDirect__FP8mgCFrame
        runtime.registerFunction(0x0012F2E0u, g15_clipbox_probe);    // mgClipBoxW (cull gate)
        runtime.registerFunction(0x0013F4E0u, g15_mdt_probe);        // Draw__12mgCVisualMDT (mesh emit)
        runtime.registerFunction(0x00145E80u, g15_sendvuprog_probe); // mgSendVuProg (VU prog upload)
    }
    if (g226_trace_enabled() || dc2_env_flag_enabled("DC2_TRACE_G227") ||
        g228_dascan_enabled() || g229_datree_enabled())
    {
        runtime.registerFunction(0x0016B940u, g226_actor_da_probe); // DrawDirect__12CActionChara (G226 DA/pendant probe; G228 captures DA frames)
    }
    // PHASE G212 FIX (default-ON, kill DC2_G212_NO_LWGUARD=1): preempt-suppression guards
    // for the joint-matrix / draw-rect windows (the G211 cap-mesh root cause). Registered
    // UNCONDITIONALLY and BEFORE the gated diagnostic blocks below, so when a trace is
    // enabled its probe overwrites the table slot -- each such probe reproduces the same
    // suppression internally (g209_lwmatrix_probe / g206_getdrawrect_probe /
    // g37_getdrawrect_probe).
    runtime.registerFunction(0x00137030u, g212_lwmatrix_guard);    // GetLWMatrix__8mgCFrame
    runtime.registerFunction(0x00143160u, g212_getdrawrect_guard); // mgGetDrawRect (covers GetDrawRect@0x1381C0's checkpoint)
    if (dc2_env_flag_enabled("DC2_TRACE_G227"))
    {
        // G227: per-outline-node census, registered AFTER the unconditional G212 guards above
        // so it wins the 0x143160 table slot (matches g206/g214/g37's established pattern).
        runtime.registerFunction(0x0017C2D0u, g227_node_probe);        // Draw__12COutLineDraw::Draw (per-node identity)
        runtime.registerFunction(0x00143160u, g227_getdrawrect_probe); // mgGetDrawRect (per-node ret/box)
    }
    if (g229_datree_enabled())
    {
        runtime.registerFunction(0x0017C2D0u, g229_outline_probe); // per-visual matrix + packet growth
        runtime.registerFunction(0x0013F4E0u, g230_visualmdt_probe); // G230: per-bucket packet/qwc sample
    }
    if (dc2_env_flag_enabled("DC2_G233_COLPROBE"))
        runtime.registerFunction(0x0017C090u, g233_colpipe_checkhit_probe); // CDAColPipe::CheckHit (DA torso push-out)
    // G214: skin-matrix collapse scanner overrides the GetLWMatrix guard slot (reproduces its
    // preempt suppression) -- samples EVERY call, not just the outline window g209 sampled.
    if (g214_lwscan_enabled() || g228_dascan_enabled() || g229_datree_enabled())
        runtime.registerFunction(0x00137030u, g214_lwscan_probe);  // GetLWMatrix (G214 collapse scan + G228 DA-frame det scan)
    if (g205_trace_enabled())
    {
        runtime.registerFunction(0x0016B850u, g205_actionchara_draw_probe); // Draw__12CActionChara (field, non-Direct)
        // G205: reuse G34's existing fVar13 capture on ANY route (not just g9_costume_enabled()),
        // since raw-body edits to recomp/DrawDirect__11CCharacter2Fv_0x1731f0.cpp were found to be
        // linker-shadowed by the duplicate definition ps2_runtime's runner/*.cpp glob force-links
        // (see phase-G205-fix-log.md) -- registerFunction wrappers are the only reliable probe point
        // for this symbol. g34_outline_draw_probe is read-only (fprintf); does not mutate state.
        runtime.registerFunction(0x0017C2D0u, g34_outline_draw_probe);  // Draw__12COutLineDraw::Draw
        runtime.registerFunction(0x001731F0u, g15_char_draw_probe);     // DrawDirect__11CCharacter2 (packet-delta)
        // G206: per-part screen bbox (read-only, no box rewrite) -- only when the costume
        // g9 block below hasn't already claimed 0x143160 for the G37 box-fix wrapper.
        if (g206_bbox_trace_enabled() && !g9_costume_enabled())
            runtime.registerFunction(0x00143160u, g206_getdrawrect_probe); // mgGetDrawRect (G206 bbox trace)
        if (g206_bbox_trace_enabled() && !g15_trace_enabled())
            runtime.registerFunction(0x0013F4E0u, g206_mdt_probe); // Draw__12mgCVisualMDT (G206 pktDelta trace)
        if (g206_bbox_trace_enabled())
            runtime.registerFunction(0x00137030u, g209_lwmatrix_probe); // GetLWMatrix (G209 skin-matrix hypothesis test)
    }
    if (dc2_env_flag_enabled("DC2_G217_EEOBJ"))
    {
        runtime.registerFunction(0x0013F4E0u, g217_mdt_draw_probe);            // direct/deferred packet route
        runtime.registerFunction(0x001404D0u, g217_base_createpacket_probe);   // plain MDT head/cap visuals
        runtime.registerFunction(0x0028A660u, g217_motion_createpacket_probe); // MotionMDT outer; base call is direct jal
        runtime.registerFunction(0x0013FF60u, g217_facepacket_probe);          // per-face geometry DMA
        runtime.registerFunction(0x0013F6A0u, g217_createpacket_probe);        // per-visual face-list chain build
        runtime.registerFunction(0x0013F920u, g217_fix_createpacket_probe);    // FixMDT prebuilt-face chain build
    }
    // G234: map material-color dump (independent of G217/G16; only one may claim 0x1404D0).
    if (dc2_env_flag_enabled("DC2_G234_MAT"))
    {
        runtime.registerFunction(0x001404D0u, g234_mat_probe);  // CreateRenderInfoPacket material colors
    }
    // PHASE G16/G30/G99: CreateRenderInfoPacket probes (quiet unless explicitly requested).
    if (g16_trace_enabled() || g30_trace_enabled() || g99_ri_trace_enabled())
    {
        runtime.registerFunction(0x001404D0u, g16_createpacket_probe);  // CreateRenderInfoPacket
    }
    if (g43_face_trace_enabled())
    {
        runtime.registerFunction(0x0013FF60u, g43_create_face_packet_probe); // G43 exact face-packet input/output dump
        runtime.registerFunction(0x0013F4E0u, g43_mdt_face_dump_probe); // G43 face-node/packet A/B dump
    }
    else if (g16_trace_enabled())
    {
        runtime.registerFunction(0x0013F4E0u, g16_mdt_listwalk_probe);  // Draw__12mgCVisualMDT deform-list walk
    }
    // G95: title copy-path TEX0 rebinding proof wrapper. Default behavior delegates
    // to the original draw loop; DC2_G95_COPY_TEX0/DC2_G95_REPLAY_DRAW enable A/B.
    runtime.registerFunction(0x00135720u, g91_drawmgr_flush_probe);
    if (g67_trace_enabled() || g95_add_mini_tex0_enabled() || g135_src_trace_enabled() ||
        dc2_env_flag_enabled("DC2_G195_BLOCK_TRACE") ||
        dc2_env_flag_enabled("DC2_G217_EEOBJ"))
        runtime.registerFunction(0x001359D0u, g67_addpacket_probe);     // G67 trace / G95 mini-packet proof / G135 source-chain trace
    if (g67_trace_enabled())
    {
        runtime.registerFunction(0x00137E10u, g67_frame_draw_probe);  // G67 title mgCFrame cull/queue trace
        runtime.registerFunction(0x00135C70u, g67_frame_test1_probe);  // G67 title distance-cull trace
        runtime.registerFunction(0x0012F2E0u, g67_frame_clipbox_probe); // G67 title clip-box cull trace
        runtime.registerFunction(0x0015E250u, g67_map_drawsub_probe);   // G67 title CMap draw-list trace
        runtime.registerFunction(0x0015E3D0u, g67_mapparts_draw_probe);  // G67 title CMapParts queue-delta trace
        runtime.registerFunction(0x00168FD0u, g67_get_texture_block_no_probe); // G67 TitleMapDraw texture-block list trace
        runtime.registerFunction(0x0013F4E0u, g67_mdt_title_probe);     // G67 title VisualMDT material/TEX0 queue trace
        runtime.registerFunction(0x00142560u, g67_mgenddraw_reload_probe); // G67 title block reload trace
        runtime.registerFunction(0x00142580u, g67_mgenddraw_probe);     // G67 title block draw trace
    }
    else if (dc2_env_flag_enabled("DC2_G195_FG_TRACE") ||
             dc2_env_flag_enabled("DC2_G195_BLOCK_TRACE") ||
             (std::getenv("DC2_G88_GEO") != nullptr))
    {
        runtime.registerFunction(0x00142580u, g67_mgenddraw_probe);     // G195 map block tag for raster traces
    }
    if (dc2_env_flag_enabled("DC2_G195_BLOCK_TRACE"))
        runtime.registerFunction(0x0013F4E0u, g195_mdt_owner_probe);     // G195 foreground MDT owner trace
    // PHASE G34: costume model RTT->display composite gap. On the costume route,
    // wrap the character draw (reads/optionally restores the +0x100 outline level)
    // and the COutLineDraw composite gate (captures fVar13). Registered last so the
    // g34 wrapper wins 0x1731f0 over the g15 trace probe when both flags are set.
    if (g9_costume_enabled())
    {
        runtime.registerFunction(0x001731F0u, g34_char_draw_probe);     // DrawDirect__11CCharacter2
        runtime.registerFunction(0x0017C2D0u, g34_outline_draw_probe);  // Draw__12COutLineDraw::Draw
        runtime.registerFunction(0x00143160u, g37_getdrawrect_probe);   // mgGetDrawRect (G37 bbox repair)
        runtime.registerFunction(0x0017CB20u, g34_divsprite_probe);     // DrawDivSprite4 (RTT composite)
        if (g206_bbox_trace_enabled() && !g16_trace_enabled() && !g43_face_trace_enabled())
            runtime.registerFunction(0x0013F4E0u, g206_mdt_probe);      // Draw__12mgCVisualMDT (G206 pktDelta trace)
        if (g36_packet_trace_enabled())
        {
            runtime.registerFunction(0x00134B00u, g36_directdata_probe); // DirectData cursor trace
            runtime.registerFunction(0x00134940u, g36_endprim2_probe);   // EndPrim2 packet-finalize trace
        }
    }
    // PHASE G204: character LOD/part trace on ANY route (town + costume). Registered
    // after the g9 block so it wins 0x1731f0 when DC2_G204_LOD_TRACE=1.
    if (g204_lod_trace_enabled())
    {
        runtime.registerFunction(0x001731F0u, g204_lod_probe);          // DrawDirect__11CCharacter2 LOD trace
    }
    // PHASE F38: FinishForMC nop_stub removed ג€” real recomp
    // FinishForMC__18CMemoryCardManagerFv_0x2f19a0 registered by
    // recomp/register_functions.cpp is left in effect. Title->menu handoff
    // proceeds through the real memory-card finalize path.
    // runtime.registerFunction(0x002F19A0u, nop_stub);                 // FinishForMC

    // PHASE F22: mgCCamera ctor ג€” set vtable + scalar fields so MenuCamInit's
    // `*(*(cam+0x60)+0x10)` virtual dispatch resolves to a real PS2 address.
    runtime.registerFunction(0x00131630u, mgccamera_ct_stub);
    // PHASE F42: mgCCameraFollow ctor ג€” auto-stub left vtable at this+0x60 as 0,
    // causing TitleMapDraw's GetCamera virtual dispatch to jump to 0x33313237.
    runtime.registerFunction(0x00131A90u, mgccamerafollow_ct_stub);
    // MenuCamInit override skips the tail-call virtual dispatch that would
    // re-enter MenuMainInit at a non-entry PC (0x232EE8).
    runtime.registerFunction(0x00234540u, menucaminit_stub);

    // PHASE F23c ג€” libc 64-bit / FP conversion helpers. Each shim implements
    // the canonical math against the verified PS2 EE single-register-per-
    // 64-bit-value ABI. The __divdi3 binding overrides the prior Group-A
    // name binding above (last-write-wins).
    runtime.registerFunction(0x002875E8u, umoddi3_stub);
    runtime.registerFunction(0x00286950u, moddi3_stub);
    runtime.registerFunction(0x00286078u, divdi3_stub);
    runtime.registerFunction(0x00287018u, udivdi3_stub);
    runtime.registerFunction(0x00286FB8u, muldi3_stub);
    runtime.registerFunction(0x00286768u, fixdfdi_stub);
    runtime.registerFunction(0x002868B8u, floatdidf_stub);

    // PHASE F25: silence empty-stem map\.{map,cfg,mpk,sky,ipk,efp} boot errors.
    // Filter at LoadFile2 (the centralised file-open wrapper per F19's
    // ra=0x149688 finding) ג€” LoadMapFile_0x164480 is never reached in the
    // current 30 s boot window so a shim at that level wouldn't fire.
    //
    // PHASE F42: re-enabled for map-entry probe iteration. F42 fixes the
    // TitleMapDraw vtable crash so the title can now progress further; the
    // [F39:lf2] probe may fire. Reverted at F42 close if still unreachable,
    // or promoted to permanent if a non-empty stem is observed.
    runtime.registerFunction(0x00149370u, loadfile2_skip_empty_map_stub);

    // PHASE F14 ג€” WaitEnable__8CGamePadFv (0x14A830) busy-waits for pad-state
    // transition via scePadGetState/read_pad. With our headless backend the
    // loop has no source of motion (scePadGetState sticky-stable at 6,
    // read_pad returns 0). Return immediately so MainLoop can advance to
    // mgEndFrame. See plans/phase-F14-fix-log.md.
    // PHASE F15: dc2_game_override.cpp is now in the ps2_runtime CMake source list.
    // The WaitEnable early-return in recomp/WaitEnable__8CGamePadFv_0x14a830.cpp
    // is still the active F14 fix (kept for clarity; identical effect to nop_stub).

    // -------------------------------------------------------------------------
    // PHASE F5 ג€” DVD path prefix fix + Exit halt
    // -------------------------------------------------------------------------
    runtime.registerFunction(0x00149240u, getfullpath_shim);
    // Exit_0x118FB0 uses a tail-j to _Exit (0x10FD00) so 0x10FD00 is a direct C++ call
    // in the runner, not a dispatch-table lookup. Bind the Exit wrapper itself instead.
    runtime.registerFunction(0x00118FB0u, exit_halt_stub);

    // PHASE G186 (opt-in DC2_G186_SPTRACE=1): sp-desync probe on the edit loop/draw.
    if (g186_sptrace_enabled())
    {
        runtime.registerFunction(0x001AE3D0u, g186_editdraw_probe); // EditDraw__Fv
        runtime.registerFunction(0x001ABCF0u, g186_editloop_probe); // EditLoop__Fv
    }
}
