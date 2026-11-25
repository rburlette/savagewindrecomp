"""Places in the game's main module that Savage Wind patches or reads, found by pattern for each game version.

    game_sites.py find <exefs/main>      every site in that main module: what its pattern finds, against the
                                         addresses known for that build (a new version: the table to add)
    game_sites.py pattern <exefs/main> <address> [code|call|ref|got|imm]
                                         a pattern that finds that one instruction (for a new site)
    game_sites.py common <main A> <address in A> <main B> <address in B> [kind]
                                         a pattern that finds the same instruction in two versions (for a site whose
                                         pattern no longer finds it: the bits both versions share)

tools/patches.py replaces single instructions at some of these sites while gen.py generates the code. The runtime
reads or writes game data at the others (runtime/game_sites.h): gen.py puts every site into the game library (the
generated layout.c), so the runtime takes the addresses of the version it runs.

A site is found by a short pattern of instruction words, so a later build of the game (other addresses, the same
code) needs no new numbers. Patterns mask the fields that change when code or data moves: branch and call offsets,
ADRP pages, and the low 12 bits an ADD / LDR / STR adds to an ADRP page in the pattern. `at` picks the instruction
(in words from the pattern's start), and the site is
    code  that instruction's own address
    call  the function a BL / B there goes to
    ref   the address an ADD / LDR / STR there forms with its ADRP (the ADRP itself: its page)
    got   the pointer stored at that `ref` address (a GOT entry; the module's relocations make it absolute)
    imm   the instruction's immediate (an LDR / STR offset, an ADD / CMP / MOVZ value): a field offset in a game
          object that a version moved (1.9.0's UI screens are 0x18 bytes bigger from about +0x290 on)
and two have no pattern: data (where the module's .data starts) and end (where its image ends, .bss included).
One pattern serves every version while the code around a site stays the same. Where a version changed it, the site
takes a list of (pattern, at) instead: the first that finds exactly one place wins (each must find one place, or
none, in every version).

The versions Savage Wind can set up are VERSIONS (the same list as runtime/game_versions.h, which the configuration
screen checks: tools/test_game_sites.py compares them). The builds in KNOWN also list every site's address: there a
pattern has to find exactly that place (the self-check), and a pattern that no longer finds exactly one place falls
back to the listed address with a warning. A build not in KNOWN needs every pattern to find exactly one place.
docs/SAVAGE-WIND.md ("Versiones del juego") has the steps for a new version.
"""
import re
import struct
import sys
from collections import namedtuple

MAIN_BASE = 0x7000000000  # (tools/layout.py: main is the first module)

# name, version, main module build id (the first 8 bytes; tools/sw_probe.py prints it), supported. One that isn't
# supported yet is known but refused by the setup until its sites are checked.
Version = namedtuple('Version', 'name build_id supported')
VERSIONS = [
    Version('1.9.0', 'cd57b23fa4bbad65', True),  # (2026-02-17: Thai text on Switch 2; SDK 21.4.1)
]

Site = namedtuple('Site', 'name kind pattern at')
BLADE_BUFFERS = '910e0135 b85fc3a9 7103fd1f 52800f18 12001d36 52801fe9 1a893108'
BLADE_LATERAL = ('bd4207e0 bd420be1 1e298821 1f0d0402 1f2d0400 1e202048 1e20cc40 bd000720 1e262000 bd4213e1 1e35d428 '
                 '54000005/ff00001f bd4217e1 1e272000')
TRAVERSE = ('b9400028 2f00e400 36600008/fff8001f d65f03c0 a9bf7bfd 910003fd 12000042 12000083 94000000/fc000000 '
            '90000008/9f00001f 91000108/ffc003ff f9400108 b4000008/ff00001f bd431101 52a85908 1e202008 1e270102 1e22cc00 '
            '1e210800 a8c17bfd 14000000/fc000000 1e2e1001')
# The game's input queries: isHold(pad, action) / isTrig(pad, action). Both load the action's pad mask from a table of 41
# (cmp w1, #0x29) and test it against the pad object's held word (+0x11c) or pressed word (+0x10)
PAD_QUERY = ('a9be7bfd a9014ff4 910003fd 7100a43f 90000008/9f00001f 91000108/ffc003ff 1a9f3029 aa0003f3 b8695914 '
             '7100229f 7a421a84 54000001/ff00001f 2a1f03e0 94000000/fc000000 71000a9f 52800048 52800109 1a880128 '
             '7200001f 1a941114')
OPT_TWO_SITES = ('opt_two_redraw', 'opt_two_select', 'opt_two_state', 'opt_two_decide', 'opt_two_after', 'opt_two_row_state', 'opt_two_update_control', 'opt_two_initial_control', 'opt_two_left', 'opt_two_right', 'opt_two_text', 'opt_two_help', 'opt_two_pane_calc')

SITES = [
    # Experimental dual binding cells. Every pattern must match; no KNOWN fallback.
    Site('opt_two_redraw', 'code', 'd10103ff a9017bfd a90257f6 a9034ff4 910043fd 90000009/9f00001f 91000129/ffc003ff b9405008', 0),
    Site('opt_two_select', 'code', 'b976b008 7100051f 5400000b/ff00001f a9be7bfd a9014ff4 910003fd 9000000b/9f00001f 9100016b/ffc003ff', 0),
    Site('opt_two_state', 'code', 'b976b008 7100051f 5400000b/ff00001f 9000000b/9f00001f 9100016b/ffc003ff f95b5c09 b940016a 5280060c f940056b f940012d b400000d/ff00001f b94051ae 6b0e015f 1a9f81ce 9b2c2dce b94011ce 710009df 54000000/ff00001f 710005df 54000000/ff00001f 3500000e/ff00001f 910061ad 14000000/fc000000 9100a1ad 14000000/fc000000 910081ad f94001ad eb0101bf 54000000/ff00001f 91002129 f1000508 54000001/ff00001f 14000000/fc000000 aa1f03ed 14000000/fc000000 d65f03c0 f9400120 17ffff20', 0),
    Site('opt_two_decide', 'code', 'b976b008 7100051f 5400000b/ff00001f 9000000b/9f00001f 9100016b/ffc003ff f95b5c09 b940016a 5280060c f940056b f940012d b400000d/ff00001f b94051ae 6b0e015f 1a9f81ce 9b2c2dce b94011ce 710009df 54000000/ff00001f 710005df 54000000/ff00001f 3500000e/ff00001f 910061ad 14000000/fc000000 9100a1ad 14000000/fc000000 910081ad f94001ad eb0101bf 54000000/ff00001f 91002129 f1000508 54000001/ff00001f 14000000/fc000000 aa1f03ed 14000000/fc000000 d65f03c0 f9400120 14000001', 0),
    Site('opt_two_after', 'code', 'b976b008 7100051f 5400000b/ff00001f 90000009/9f00001f 91000129/ffc003ff f95b5c0a b940012b 5280060c', 0),
    Site('opt_two_row_state', 'code', '90000008/9f00001f 91000108/ffc003ff b9405009 b940010a 6b09015f f9400508 5280060a 1a9f8129 9b2a2128', 0),
    Site('opt_two_update_control', 'code', 'f9400129 eb09011f 54000000/ff00001f b976c268 71000d1f 54000001/ff00001f f9401e68 3940e108', 0),
    Site('opt_two_initial_control', 'code', 'f9400161 b4000001/ff00001f aa1303e0 94000000/fc000000 aa0003e1 aa1303e0 94000000/fc000000 b9405288', 0),
    Site('opt_two_left', 'code', 'b936c268 f95b6668 f940068b b9405109 6b09015f 5280060a 1a9f8129 9b2a2d29 b9401129 34000009/ff00001f 7100053f 54000001/ff00001f b9405909', 0),
    Site('opt_two_right', 'code', 'b936c268 f95b6668 f940068b b9405109 6b09015f 5280060a 1a9f8129 9b2a2d29 b9401129 34000009/ff00001f 7100053f 54000001/ff00001f 294b250a', 0),
    Site('opt_two_text', 'code', 'b4000000/ff00001f d10583ff a9137bfd a91457fc a9154ff4 9104c3fd 90000009/9f00001f 910063e8', 0),
    Site('opt_two_help', 'code', 'd100c3ff a9017bfd f90013f3 910043fd aa0003f3 94000000/fc000000 90000009/9f00001f 91000129/ffc003ff', 0),
    Site('opt_two_pane_calc', 'code', 'd10243ff fd0023e8 f9002ff9 a904fbfd a9065ff8 a90757f6 a9084ff4 910123fd 39416008', 0),
    # The Options screen (tools/patches.py; runtime/ui/game_options.mm)
    Site('opt_create_rows', 'code', 'd10303ff a9067bfd a9076ffc a90867fa a9095ff8 a90a57f6 a90b4ff4 910183fd '
         '90000008/9f00001f b9400118/ffc003ff', 0),
    Site('opt_row_init_flag_type', 'code', '94000000/fc000000 b9006a60 aa1303e0 94000000/fc000000 aa1303e0 '
         '52800021 94000000/fc000000 a9424ff4', 0),
    Site('gdt_flag_type', 'call', '94000000/fc000000 b9006a60 aa1303e0 94000000/fc000000 aa1303e0 52800021 '
         '94000000/fc000000 a9424ff4', 0),
    Site('opt_row_redraw', 'call', 'aa1303e0 52800021 94000000/fc000000 a9424ff4 a9417bfd 9100c3ff d65f03c0', 2),
    Site('opt_read', 'code', 'a9bd7bfd f9000bf5 a9024ff4 910003fd 90000008/9f00001f 91000108/ffc003ff b9405009 '
         'b940010a', 0),
    Site('opt_store', 'code', '90000008/9f00001f 91000108/ffc003ff b9405009 b940010a 6b09015f f9400508 5280060a '
         '1a9f8129 9b2a2121', 0),
    Site('opt_table', 'ref', '90000008/9f00001f 91000108/ffc003ff b9405009 b940010a 6b09015f f9400508 5280060a '
         '1a9f8129 9b2a2121', 1),
    Site('opt_reset', 'code', 'b9406808 7100051f 54000000/ff00001f 35000008/ff00001f 90000008/9f00001f '
         '90000009/9f00001f 91000129/ffc003ff f9400108/ffc003ff', 0),
    Site('opt_msg_call', 'code', '94000000/fc000000 910023e2 aa1403e0 aa1303e1 94000000/fc000000 a9554ff4 a95457fc '
         'a9537bfd', 0),
    Site('msg_lookup', 'call', '94000000/fc000000 910023e2 aa1403e0 aa1303e1 94000000/fc000000 a9554ff4 a95457fc '
         'a9537bfd', 0),
    Site('opt_decide', 'code', 'd100c3ff a9017bfd a9024ff4 910043fd 90000009/9f00001f 91000129/ffc003ff b9405008 '
         'b940012a 5280060b', 0),
    Site('opt_update', 'code', 'd10103ff a9017bfd a90257f6 a9034ff4 910043fd 5286d288/fffff2ff aa0003f3 8b080015', 0),
    Site('opt_back_call', 'code', '94000000/fc000000 f9401e68 b9403909 121e7929 b9003909 90000008/9f00001f '
         '91000108/ffc003ff f9400108', 0),
    Site('screen_back', 'call', '94000000/fc000000 f9401e68 b9403909 121e7929 b9003909 90000008/9f00001f '
         '91000108/ffc003ff f9400108', 0),
    Site('screen_open_request', 'call', '94000000/fc000000 90000008/9f00001f f9400108/ffc003ff f9400108 b9402909 '
         'f9401908 7100d53f 52803509', 0),
    Site('screen_open', 'code', 'a9be7bfd a9014ff4 910003fd aa0003f4 390a001f/ffff1fff b940c000 2a0103f3 '
         '94000000/fc000000', 0),
    Site('screen_close', 'call', '94000000/fc000000 52800028 0a330100 a9414ff4 a8c27bfd d65f03c0', 0),
    Site('screen_mgr', 'ref', '90000008/9f00001f 91000108/ffc003ff f9400108 b9402909 f9401908 7100913f 52802409 '
         '9a9f8129', 1),
    Site('pause_options_update', 'code', 'a9be7bfd a9014ff4 910003fd 90000008/9f00001f aa0003f3 f9400108/ffc003ff '
         'f9400108 b9402909 f9401908 7100d13f 52803409 9a9f8129 f8696914 94000000/fc000000 b4000014/ff00001f '
         'f9400288 aa0003e1 aa1403e0 f9400908 d63f0100 36000000/fff8001f 394a0688/ffff1fff', 0),
    Site('opt_screen_rows', 'imm', 'b9400008/ffc003ff 7100051f 5400000b/ff00001f f95b5009/fffff3ff f9400120 '
         'b940500a 6b01015f 54000000/ff00001f 91002129 f1000508 54000001/ff00001f d65f03c0 52800021', 0),
    # the game's two-button dialog (Quit asks with it)
    Site('dialog_texts', 'code', 'd10143ff a9017bfd f90013f7 a90357f6 a9044ff4 910043fd d2800b49 5286d888/fffffcff', 0),
    Site('dialog_back', 'code', 'f959c100/ffffc3ff b4000000/ff00001f a9414ff4 2a1f03e1 a8c27bfd 14000000/fc000000 '
         'f9400268 aa1303e0', 0),
    Site('dialog_answer', 'ref', '90000008/9f00001f 528001a9 aa0003f3 b9000109/ffc003ff 94000000/fc000000 aa0003e1 '
         'aa1303e0 94000000/fc000000', 3),
    # the title screen menu
    Site('title_update', 'code', 'a9bb7bfd a90167fc 910003fd a9025ff8 a90357f6 a9044ff4 d107c3ff 5286c208/fffff2ff', 0),
    Site('title_button', 'code', 'd10183ff a9037bfd f90023f5 a9054ff4 9100c3fd b9404434 aa0003f3 94000000/fc000000', 0),
    Site('title_options_state', 'imm', 'b9400268/ffc003ff 5101a508 7100191f 54000008/ff00001f 90000009/9f00001f '
         '91000129/ffc003ff 1000000a/9f00001f 3868692b', 0),
    Site('title_msg_file', 'ref', 'f9000549 90000009/9f00001f 91000129/ffc003ff 9000000a/9f00001f '
         'f900015f/ffc003ff a9002d28 a8c17bfd d65f03c0', 2),
    # the pause menu's System page: its button handler (buttons 0x4d..0x53; the seventh, 0x53, is Achievements)
    Site('pause_button', 'code', 'b9404428 51013508 7100191f 54000009/ff00001f d65f03c0 d10183ff a9037bfd f90023f5', 0),
    # ... and where its setup reads whether to add a seventh button (0x53, the Nintendo Switch 2 Edition upgrade, made
    # at run time as Pa_Btn_06 in the gap the layout leaves): offered (+0x280) and not owned (+0x281). It is
    # Achievements.
    Site('pause_upgrade_offer', 'code', '394a0268 34000008/ff00001f 394a0668 34000008/ff00001f 910703ff a9434ff4 a94257f6 '
         'a9415ffc', 0),
    Site('pause_upgrade_owned', 'code', '394a0268 34000008/ff00001f 394a0668 34000008/ff00001f 910703ff a9434ff4 a94257f6 '
         'a9415ffc', 2),
    # ... and where that setup gives the button its text, "Nintendo Switch 2 Edition" (a string in the code, not a message):
    # `mov x1, sp` before the call that sets it ({char16_t *, u32 length} at sp)
    Site('pause_upgrade_text', 'code', '910003e1 aa1303e0 aa1f03e2 94000000/fc000000 14000000/fc000000 f94002c8 aa0003f7 '
         'aa1603e0', 0),
    # ... and the message files' search by label (x0 = file, x1 = label, x8 = {char16_t *, u32 length}), which the
    # layouts' texts go through: that button's text
    Site('msg_find', 'code', 'a9bd7bfd a90157f6 a9024ff4 910003fd aa0003f4 f9400400 aa0803f3 94000000/fc000000', 0),
    Site('title_version_format', 'ref', '90000001/9f00001f 91000021/ffc003ff d103c3a0 94000000/fc000000 '
         'f9400294/ffffffdf b940da88 35000008/ff00001f 90000001/9f00001f', 1),
    # BOTW's streaming Yaz0 decoder: its entry and its return (tools/patches.py; runtime/hle/yaz0.c)
    Site('yaz0_stream', 'code', 'a9bd7bfd f9000bf5 a9024ff4 910003fd 39406808 2a0203f4 aa0103f5 aa0003f3', 0),
    Site('yaz0_stream_ret', 'code', 'd65f03c0 7100869f 54000003/ff00001f b9400e68 35000008/ff00001f 12800000 '
         '14000000/fc000000 12800020', 0),
    # frame rate (runtime/hle/framepace.c, runtime/nvn/objects.c)
    # the title screen's version line (tools/patches.py _title_version_text): the load of System/Version.txt's string
    # for "Ver.%s", at the title's start and at its refresh
    Site('title_version_text_a', 'code', 'f9408682 90000001/9f00001f 91024821 910063e0 94000000/fc000000', 0),
    Site('title_version_text_b', 'code', 'f94086a2 90000001/9f00001f 91024821 d103c3a0 94000000/fc000000', 0),
    Site('frame_wait', 'code', 'a9bd7bfd f9000bf5 a9024ff4 910003fd 91078014 aa0003f3 aa1403e0 94000000/fc000000', 0),
    Site('frame_loop_interval_ret', 'code', 'f9400268 aa1303e0 f9409908 d63f0100 14000000/fc000000 1100052a '
         '14000000/fc000000 2a0903e1', 4),
    Site('vfr_ratio', 'code', 'b9407a68 7100051f 5400000b/ff00001f 2a1f03f4 f9404269 6b14011f 1a9f8288 bd403e60', 0),
    # the menus' speed (tools/patches.py _gui_speed): the frame interval x 0.5 stored as the GUI speed
    # perspective projections' matrix (tools/patches.py _persp_matrix): the load of tan(fovy / 2) and the aspect ratio
    Site('persp_matrix', 'code', 'bd409800 1e2c1003 52b7f008 2d558801 1e202800 2d569404', 3),
    Site('persp_matrix_offset', 'code', 'bd409800 1e2c1003 52b7f008 2d558801 1e202800 2d569404', 5),
    # frustum projections' matrix (tools/patches.py _frustum_matrix): its four loads of left / right and top / bottom
    Site('frustum_lr0', 'code', '2d550001 52b7f008 bd409802 b900043f 1e213800 1e2e1001 1e222842 1e201820', 0),
    Site('frustum_lr1', 'code', '2d550001 52b7f008 bd409802 b900043f 1e213800 1e2e1001 1e222842 1e201820', 10),
    Site('frustum_tb0', 'code', '2d550001 52b7f008 bd409802 b900043f 1e213800 1e2e1001 1e222842 1e201820', 15),
    Site('frustum_tb1', 'code', '2d550001 52b7f008 bd409802 b900043f 1e213800 1e2e1001 1e222842 1e201820', 23),
    # projections' update (tools/patches.py _proj_update): the load of the matrix's dirty flag
    Site('proj_update', 'code', '39402008 aa0003f3 34000268 f9400268 91003261 aa1303e0 f9402d08 d63f0100', 0),
    # perspective projections' screen position to camera position (tools/patches.py _persp_unproject): the loads of
    # tan(fovy / 2) (y) and of it with the aspect ratio (x)
    Site('persp_unproject_y', 'code', 'bd409800 f900003f 1e201004 1e214000 bd000820 bd409800 bd40ac01 bd400442', 6),
    Site('persp_unproject_x', 'code', 'bd409800 f900003f 1e201004 1e214000 bd000820 bd409800 bd40ac01 bd400442', 18),
    # TimeMgr's clock step in OnlyUpdateTimeOfDay and normal mode, respectively.
    Site('day_cycle_time_only', 'code', '90000008/9f00001f bd40c668 f9400108/ffc003ff f9400100 94000000/fc000000 '
         'bd40ba61 52a87688 1f000500', 1),
    Site('day_cycle_normal', 'code', '90000008/9f00001f bd40c668 f9400108/ffc003ff f9400100 94000000/fc000000 '
         '1e200908 bd40ba60 52800028 39053e68', 1),
    # arrows' fall (tools/patches.py _arrow_gravity): the load of the gravity added to an arrow's velocity per update
    Site('arrow_fall_gravity', 'code', 'bd410a81 1e212849 bd410e81 1e21280a 94000000/fc000000 36000000/fff8001f f9404288 2d400500', 0),
    # Experimental glide decay (tools/patches.py _glide_overspeed_dec): the paraglider's stick-gated update subtracts
    # its OverSpeedDec parameter from velocity +0x181c / +0x1824 once per guest update, with no time step. The first
    # three words are identical at both sites; the store word tells them apart. `at` 2 is the fsub at +8.
    Site('glide_overspeed_dec_181c', 'code', 'f9403668 bd400101 1e213800 bd181e80', 2),
    Site('glide_overspeed_dec_1824', 'code', 'f9403668 bd400101 1e213800 bd182680', 2),
    # The fadd mirrors: with a non-positive target the guest ADDS OverSpeedDec instead of subtracting it. The fadd
    # word is the same at both sites and the following branch back to the shared store cannot tell them apart, so
    # the distinguishing fcmp word four words earlier is part of the pattern. `at` 4 is the fadd.
    Site('glide_overspeed_add_181c', 'code', '1e2e2000 540000a5 f9403668 bd400101 1e212800', 4),
    Site('glide_overspeed_add_1824', 'code', '1e2d2000 540000a5 f9403668 bd400101 1e212800', 4),
    # Experimental shield-surf takeoff grace (#305): contact update, boost removal and launch request.
    Site('surf_contact', 'code', '3941d2a8 36080008/fff8001f bd4066a0 1e202008 5400000d/ff00001f bd403261 1e202028 5400000d/ff00001f', 0),
    Site('surf_boost_remove', 'code', '1e203820 2f00e401 aa1403e1 1e216800 bd003260 0f016680 f94006a0 94000000/fc000000', 0),
    Site('surf_jump_request', 'code', '36000000/fff8001f 52a82608 b9006668 b9008288 52800028 3900a288 2d488680 52800141', 0),
    # a stale actor in the target manager (tools/patches.py _stale_actor_helper, _stale_actor_target; #391, #393): the
    # helper that asks an actor for its link, and the camera target's inline copy of it
    Site('stale_actor_helper', 'code', 'b4000000/ff00001f f942b808 f9404500 14000000/fc000000 d65f03c0', 0),
    Site('stale_actor_helper2', 'code', 'b4000000/ff00001f f942b808 528071a1 72a900c1 f9404500 14000000/fc000000 d65f03c0', 0),
    Site('stale_actor_kind', 'code', 'b4000000/ff00001f f942b808 aa0003f3 528ebfc1 72a499a1 f9404500 94000000/fc000000', 0),
    Site('stale_actor_target', 'code', 'f942ba68 f9404500 94000000/fc000000 12000008 390006e8 37000000/fff8001f 394002e8 35000008/ff00001f', 0),
    # and the loads of an actor's link in the check of one of its flags by name (1.9.0 0x700120c760; #766, #1011)
    Site('stale_actor_flag', 'code', 'f942ba68 52864261 72b85581 f9404500 94000000/fc000000 36000000/fff8001f 52800020 a9424ff4', 0),
    Site('stale_actor_flag2', 'code', 'f942ba68 528120a1 72a6b2c1 f9404500 94000000/fc000000 2a000288 37000008/fff8001f f942ba68', 0),
    Site('stale_actor_flag3', 'code', 'f942ba68 528071a1 72a900c1 f9404500 94000000/fc000000 14000000/fc000000 52800034 14000000/fc000000', 0),
    Site('stale_actor_flag4', 'code', 'f942ba68 528cfa41 72ad8a41 f9404500 94000000/fc000000 36000000/fff8001f 14000000/fc000000', 0),
    # AI message send to an actor link whose actor may be null (1.9.0 0x7000bbb670; #1130)
    Site('stale_actor_ai_msg', 'code', 'f942f834 3900501f f9400415 b4000015/ff00001f aa0003f6 2a0203f3 f84106c8 f9400908 d63f0100 f94002a8 aa0003e3 12000264 aa1503e0 aa1403e1 f9400905', 0),
    # the physics step's solver microsteps (tools/patches.py _solver_steps): the delta frame times 6
    Site('phys_solver_steps', 'code', '1e231002 1e3c1004 f9401a68 1e2c1009 1e220822 f9400108 1e202048 f9400514', 4),
    # Havok's deactivation at the console's pace (tools/patches.py _phys_tick, _phys_motion_tick): the world step's
    # deactivation counter (+0x3ef; the single-threaded and the multithreaded simulation) and each body's (hkpMotion +0xd;
    # the integration of islands without constraints and with them)
    Site('phys_tick', 'code', '394fbd0a 11000549 1100154a 390fbd09 1200094a 3500000a/ff00001f 394fb50a 5200014a '
         '390fb50a 1200092a 3500000a/ff00001f 394fb50a 521f014a 390fb50a 12000d29 35000009/ff00001f 394fb909 320003ea '
         '4b090149 390fb909 390fbd1f d503201f', 0),
    Site('phys_tick_mt', 'code', '394fbd0a 11000549 1100154a 390fbd09 1200094a 3500000a/ff00001f 394fb50a 5200014a '
         '390fb50a 1200092a 3500000a/ff00001f 394fb50a 521f014a 390fb50a 12000d29 35000009/ff00001f 394fb909 320003ea '
         '4b090149 390fb909 390fbd1f f9400a60', 0),
    Site('phys_motion_tick', 'code', '39403688 11000508 39003688 12000509 35000009/ff00001f 12000d09 '
         '34000009/ff00001f aa1f03e8', 0),
    Site('phys_motion_tick_c', 'code', '39403748 11000508 39003748 12000509 35000009/ff00001f 12000d09 '
         '34000009/ff00001f aa1f03e8', 0),
    # a keyframed body's angular velocity toward its target (tools/patches.py _kf_angle): the call that takes the angle
    # of the turn left (hkQuaternion::getAngle, 2 acos(|w|))
    # a player demo move's arrival test (tools/patches.py _demo_move_arrive): the distance left against the body's speed
    # in console frames, fcmp s8, s9
    Site('demo_move_arrive', 'code', 'bd60bd09 94000000/fc000000 f9400668 5282fe09 38696908 34000008/ff00001f 1e292100 '
         '54000004/ff00001f 90000008/9f00001f bd404261 bd425102 1e222821 1e282020 54000005/ff00001f 1e292921 1e200820', 6),
    # the radius within which a player rail move's point goes on along the rail (tools/patches.py _rail_point_radius):
    # its load from the player, ldr s0, [x8, #0x20f0]
    Site('rail_point_radius', 'code', 'bd60f100 1e202020 54000005/ff00001f 1e2c1001 f9403668 aa1403e0 aa1f03e1 aa1f03e2', 0),
    Site('kf_angle', 'code', '0e612800 0e0e3c08 0e040d00 0f3f5400 0ea0a800 6e180400 4e221c00 6e211c00 3c9983a0 '
         '94000000/fc000000 3cd883a6 1e2008c0', 9),
    # the sound system's frame (tools/patches.py _snd_frame_floor): the delta frame, at least 0.5, its timers' step
    Site('snd_frame_floor', 'code', '1e2c1001 1e212000 1e204c28 94000000/fc000000 b4000000/ff00001f bd415660 '
         'aa0003f4 7e21d800', 0),
    Site('gui_speed', 'code', 'bd0b2100 94000000/fc000000 f9400260 94000000/fc000000 52800020 94000000/fc000000 '
         'a9414ff4 a8c27bfd', 0),
    # MainShortCut's HoldShortCutBg: repair P_Capture_00's alpha after a fractional opening frame (#173).
    Site('quick_menu_capture_alpha', 'code', 'f9400268/ffc003ff bd405100 1e202008 54000000/ff00001f '
         'bd402100 1e2e1001 1e212000 54000001/ff00001f 90000001/9f00001f 91000021/ffc003ff '
         'aa1303e0 94000000/fc000000 52801fe8 39016408', 7),
    # Scalable font engine (CJK/Chinese TrueType) resolution scaling
    Site('cjk_font_size', 'code', '53103d01 2a0103e4 94000000/fc000000 f9400660 52800102 b9403381 94000000/fc000000 aa0003f4', 0),
    Site('cjk_font_bin', 'code', '7d406f81 4b080128 f94c826a 79406b8b 9bba296a bd400940 7e21d821 1f012400', 0),
    Site('cjk_font_quad', 'code', '2d42212b 79c03aa9 1e220120 1e200960 1e380009 79009389 79c00329 0b090289', 0),
    # the wait for the ground (tools/patches.py _tp_gate): is the map around Link not loaded yet? (the world pauses for it)
    Site('tp_gate', 'code', 'd10283ff fd002be8 f90037f9 a905fbfd a9075ff8 a90857f6 a9094ff4 910163fd '
         '90000016/9f00001f', 0),
    Site('pause_state', 'ref', '90000013/9f00001f b9400268/ffc003ff 35000008/ff00001f 90000008/9f00001f '
         'b9400108/ffc003ff 35000008/ff00001f b9400268/ffc003ff 34000008/ff00001f', 1),
    Site('event_mgr', 'ref', '9000000d/9f00001f 910001ad/ffc003ff f94001a0 aa1703e1 94000000/fc000000 aa1703e0 '
         '2a1f03e1 94000000/fc000000', 1),
    Site('controller_step', 'got', '90000008/9f00001f aa1f03f5 f9400108/ffc003ff 52800036 bd400108 b9411668 '
         '1ad522d8 8b150a77', 2),
    # (test tools only: the clock probe)
    Site('player_info', 'ref', '90000008/9f00001f 91000108/ffc003ff f9400100 b4000000/ff00001f 94000000/fc000000 '
         '7100001f 5400000c/ff00001f 91314260', 1),
    Site('world_mgr', 'ref', '90000008/9f00001f 91000108/ffc003ff f9400108 b4000008/ff00001f b946b908 7100051f '
         '54000001/ff00001f a9bf7bfd', 1),
    # the noise setter (tools/patches.py _silent_link; runtime/assists.c): a (object, channel, value) for one of
    # an actor's four noise channels, channel 1 being the one whistling and the sounds like it go through
    Site('silent_link', 'code', '51000429 91006008 7100053f 540000c8/ff00001f 8b214908 bd400101 1e202020 '
         '540000a4/ff00001f', 0),
    # the four champion abilities' "can this be used" checks (tools/patches.py _hero_soul_*; runtime/assists.c
    # rt_hero_soul): each returns 0 while the game forbids hero souls, as the shrines and the Trial of the Sword do, and
    # while the ability recharges. The patterns run from a check's entry to the call or the load that tells it apart.
    Site('hero_soul_wind', 'code',
         'f0008468/9f00001f d0008469/9f00001f f9420d08/ffc003ff f944c929/ffc003ff 39400108/ffc003ff '
         '39400129/ffc003ff 2a090108 34000068/ff00001f 2a1f03e0 d65f03c0 a9be7bfd f9000bf3 910003fd '
         'aa0003f3 2a1f03e0 94017619/fc000000 2a0003e8 2a1f03e0 36000088/ff00001f f9400bf3/ffc003ff '
         'a8c27bfd d65f03c0 aa1303e0 94003761/fc000000', 0),
    Site('hero_soul_fire', 'code',
         'f0008468/9f00001f d0008469/9f00001f f9420d08/ffc003ff f944c929/ffc003ff 39400108/ffc003ff '
         '39400129/ffc003ff 2a090108 34000068/ff00001f 2a1f03e0 d65f03c0 a9be7bfd f9000bf3 910003fd '
         'aa0003f3 2a1f03e0 940175f3/fc000000 2a0003e8 2a1f03e0 36000088/ff00001f f9400bf3/ffc003ff '
         'a8c27bfd d65f03c0 94069c50/fc000000 360000c0/ff00001f bd5e0660', 0),
    Site('hero_soul_electric', 'code',
         'f0008468/9f00001f d0008469/9f00001f f9420d08/ffc003ff f944c929/ffc003ff 39400108/ffc003ff '
         '39400129/ffc003ff 2a090108 34000068/ff00001f 2a1f03e0 d65f03c0 a9be7bfd f9000bf3 910003fd '
         'aa0003f3 2a1f03e0 940175d4/fc000000 2a0003e8 2a1f03e0 36000088/ff00001f f9400bf3/ffc003ff '
         'a8c27bfd d65f03c0 94069c45/fc000000 360000c0/ff00001f bd5e1260', 0),
    Site('hero_soul_water', 'code',
         'a9be7bfd f9000bf3 910003fd aa0003f3 2a1f03e0 39712268/ffc003ff 370001e8/ff00001f f0008468/9f00001f '
         'd0008469/9f00001f f9420d08/ffc003ff 39400108/ffc003ff f944c929/ffc003ff 39400129/ffc003ff 7100011f '
         '7a400920 540000c1/ff00001f 2a1f03e0 940175b3/fc000000 2a0003e8 2a1f03e0 36000088/ff00001f '
         'f9400bf3/ffc003ff a8c27bfd d65f03c0 94069c38/fc000000', 0),

    # the mouse camera (tools/patches.py _cam_stick, _cam_pitch, _cam_pitch_aim; runtime/hle/input.c): the camera's
    # stick input, which every camera action reads, and the square that the chase-like cameras apply to its vertical
    # part
    Site('cam_stick', 'code', 'd100c3ff fd000be8 f90017f3 a901fbfd 910063fd aa0003f3 f900001f 52800060', 0),
    # (the end of the camera's stick input, with the stick's value written: tools/patches.py _cam_stick_end)
    Site('cam_stick_end', 'code', 'f9409408 910023e0 f90007e8 94000000/fc000000 360000c0 fd400be8 f94017f3', 5),
    # CameraHorse's return address after calling the camera stick input (rt_cam_stick's lr, runtime/hle/input.c)
    Site('horse_cam_lr', 'code', 'bd401ba0 1e202008 39416a68 54000001/ff00001f bd401fa0 1e202008 54000001/ff00001f 37180008/fff8001f', 0),
    Site('cam_pitch_chase', 'code', '1e208803 1e202008 bd41ce80 1e23cc2b bd41d281 1e212000 54000001/ff00001f '
         '52800039', 0),
    Site('cam_pitch_surf', 'code', '1e208803 1e202008 bd430260 1e23cc2d bd430661 1e212000 54000001/ff00001f '
         '2f00e400', 0),
    Site('cam_pitch_wakeboard', 'code', '1e208803 1e202008 bd433260 1e23cc2f bd433661 1e212000 54000001/ff00001f '
         '2f00e400', 0),
    # CameraAiming (the bow, its zoom, the scope): the load of its latStickScale for the vertical part
    Site('cam_pitch_aim', 'code', 'f9400268/ffc003ff 1e200923 910083e0 910063a1 bd400101 1e210860 bd4057e1 1e210809',
         4),
    # ... and CameraAiming2's (the camera for a held item)
    Site('cam_pitch_item', 'code', '94000000/fc000000 1e20400b 94000000/fc000000 f9400268/ffc003ff 1e200960 bd400101 '
         '90000008/9f00001f 1e210800', 5),
    # the cameras' controller turning its right stick around for the game's Camera X-axis / Y-axis options (Inverted:
    # the flags CameraRLReverse, CameraUpDownReverse): the calls that read them
    Site('cam_reverse_x', 'code', '94000000/fc000000 36000000/fff8001f bd413260 1e214000 bd013260 2a1f03e0 '
         '94000000/fc000000 36000000/fff8001f', 0),
    Site('cam_reverse_y', 'code', '94000000/fc000000 36000000/fff8001f bd413660 1e214000 bd013660 a9424ff4 a9417bfd '
         '9100c3ff', 0),
    Site('pc_button_group', 'code', 'd10183ff a9017bfd a90267fa a9035ff8 a90457f6 a9054ff4 910043fd '
         'aa0003f3 f90003e0 f9401800 2a0403f8 2a0303f5 2a0203f6 aa0103f4 b9000bff', 0),
    Site('pc_cursor_manager', 'code', 'd10143ff a9017bfd a9025ff8 a90357f6 a9044ff4 910043fd b9000be1 91006008', 0),
    Site('pc_cursor_activate', 'code', 'a9bd7bfd f9000bf5 a9024ff4 910003fd f9402415 aa0103f3 aa0003f4 f9002401', 0),
    Site('pc_pane_bounds', 'code', 'bd407020 2d4a0821 1e200824 bd408423 1e208825 1e230841 1e238842 1e2c1003', 0),
    # Controls > Menus wrap around (runtime/menu_wrap.c): the UI focus search's result (`mov x0, x20` before its
    # epilogue; x19 the control, w22 the direction), and the screen's input layer (the search's own first call)
    Site('menu_wrap', 'code', 'aa1403e0 6d442beb 6d4333ed 6d423bef a94b4ff4 a94a57f6 a9495ff8 a94867fa', 0),
    Site('ui_control_layer', 'code', 'f9401408 3943f001 f9402508 f9400109 aa0803e0 f9401522 d61f0040', 0),
    # Map focus helpers: retain marker targeting without controller-style recentering on hover.
    Site('beast_focus_axes', 'code', 'a9bd7bfd f9000bf5 a9024ff4 910003fd 91404408 2a0103f3 aa0003f4 91231115', 0),
    Site('beast_focus_linear', 'code', 'a9bd7bfd a90157f6 a9024ff4 910003fd 91404408 2a0103f3 aa0003f4 911f2116', 0),
    Site('beast_confirm_axes', 'code', 'a9ba7bfd a9016ffc a90267fa a9035ff8 a90457f6 a9054ff4 910003fd 52831888', 0),
    Site('beast_confirm_linear', 'code', 'fc1b0fe8 a900fbfd f9000ff9 a9025ff8 a90357f6 a9044ff4 910023fd 90000018/9f00001f 91000318/ffc003ff', 0),
    Site('beast_clamp_pitch', 'code', 'd101c3ff a9057bfd f90033f3 910143fd 52800409 39012fff 910023e8 b9002be9', 0),
    Site('beast_wrap_yaw', 'code', 'b9762808 7100051f 54000000/ff00001f 35000008/ff00001f bd767400 90000008/9f00001f 1e202008 bd4aa101', 0),
    Site('beast_world', 'got', '90000009/9f00001f 52898708 72a000c8 f9400129/ffc003ff aa0103f4 f9400129 aa0003f3 b8686928', 3),
    Site('pc_map_focus', 'code', 'a9be7bfd f9000bf3 910003fd bd786800 aa0003f3 910063a1 bd786c01 91214000', 0),
    Site('pc_map_focus_target', 'code', 'd100c3ff a9017bfd a9024ff4 910043fd aa0103f3 aa0003f4 97ffffdc aa1403e0', 0),
    Site('pc_map_focus_now', 'code', 'd100c3ff a9017bfd a9024ff4 910043fd aa0103f3 aa0003f4 97ffffb3 aa1403e0', 0),
    # placed actors' traverse distance (tools/patches.py _traverse_scale): the scale the game multiplies every one by,
    # loaded from the actor debug manager, or 1 without it (Video > Draw distance)
    Site('traverse_scale', 'code', TRAVERSE, 13),
    Site('traverse_scale_default', 'code', TRAVERSE, 21),
    # the grass's view context (tools/patches.py _grass_range): its entry, with the two screen-size limits in s1, s2
    Site('grass_range', 'code', 'bd400024 bd000004 bd400425 bd000405 bd400825 2d011005 bd001005', 0),
    # ... and each layer's store of the camera scale its distance fades are measured with (_grass_fade)
    Site('grass_fade_blades', 'code', 'bd417349 aa1a03e0 94000000/fc000000 1e202120 a91ceffa 90000008/9f00001f '
         '1e20cd20 bd024360', 7),
    Site('grass_fade_quads', 'code', 'bd4172e9 aa1703e0 94000000/fc000000 1e202120 90000008/9f00001f 1e20cd20 '
         'bd000700', 6),
    # the blades' draw slots (tools/patches.py _grass_blade_slots): how many their manager makes, and the two passes
    # that stop at the 256th cell
    Site('grass_blade_slots', 'code', 'a9416ffc 52802001 52800103 aa1c03e0 aa1b03e2 94000000/fc000000', 1),
    Site('grass_blade_cap_a', 'code', '7100013f 7a401940 54000000/ff00001f 7103fe7f 54000008/ff00001f 71001d9f', 3),
    Site('grass_blade_cap_b', 'code', '79402509 34000009/ff00001f 79402908 34000008/ff00001f 7103ff7f 54000008/ff00001f '
         '7d4032c0', 4),
    # ... and the per-frame pass that points each slot at this frame's half of its shader block (count clamped to 256)
    Site('grass_blade_cap_c', 'code', BLADE_BUFFERS, 2),
    Site('grass_blade_cap_c_max', 'code', BLADE_BUFFERS, 5),
    # the blades' region test for each cell, near and far parts (tools/patches.py _grass_blade_lateral)
    Site('grass_blade_lateral_near', 'code', BLADE_LATERAL, 8),
    Site('grass_blade_lateral_far', 'code', BLADE_LATERAL, 13),
    # the terrain's distant trees (tools/patches.py _tree_ranges): the entry of their per-frame shader setup
    Site('tree_ranges', 'code', '90000008/9f00001f 91000108/ffc003ff f9400108 b4000008/ff00001f f9452908 '
         'b4000008/ff00001f 39406108 37280008/fff8001f 39402008 36000008/fff8001f d10143ff', 0),
    # a shrine apparatus fetching its input (tools/patches.py _gyro_scene_actor; runtime/pc_gyro_scene.c)
    Site('gyro_scene_actor', 'code', '2d59b28f 2d5ab689 94000000/fc000000 b5000000/ff00001f 9101d280 9101a281', 2),
    # per-action bindings (tools/patches.py _act_query; runtime/hle/bindings.c): the two queries and the mask table
    Site('act_hold', 'code', PAD_QUERY + ' b9411e68', 0),
    Site('act_trig', 'code', PAD_QUERY + ' b9401268', 0),
    Site('act_masks', 'ref', PAD_QUERY + ' b9401268', 5),
    # the store of an equipped weapon's, bow's or shield's durability, x100 (tools/patches.py _no_wear; from #296: the
    # setter 1.8.2 0x7000c809e8 finds the equipped item of a type w22 in the pouch and stores the new value w20)
    Site('no_wear_store', 'code', 'b9002174 5288fb08 72a00088 38686aa8 35000008/ff00001f 2a1403e0 2a1f03e2 '
         '94000000/fc000000', 0),
    # the weapon actor's own durability, x100 (tools/patches.py _no_wear_hit): a hit takes its cost w9 off the value w8
    Site('no_wear_hit', 'code', '4b090108 0aa87d08 1e220101 b9000268 1e212000 1a9f07e0 14000000/fc000000 f9400288', 0),
    # where the weapon's update calls that (no_wear_hit's function, a virtual method that every damage goes through):
    # the instruction after the call, the return address that tells a weapon's wear from damage to Link or an enemy
    Site('no_wear_ret', 'code', 'f9400288 2a0003f5 aa1403e0 f9415d08 d63f0100 b5000000/ff00001f 52800036 '
         '14000000/fc000000', 0),
    # Link's damage handling storing his new life, in quarter hearts (tools/patches.py _link_life): at its end, and on
    # the path that takes one quarter off
    Site('link_life_store', 'code', 'b90002b9 f9400300 94000000/fc000000 6b00033f 5400000d/ff00001f f9400300 b9400ff5 '
         '94000000/fc000000', 0),
    Site('link_life_tick', 'code', 'b90002a8 1e212000 1a9f07e0 14000000/fc000000 2a1f03fa 39715288 36080008/fff8001f '
         'bd42e660', 0),
    # the stamina setter's store, PlayerInfo +0xa4 (tools/patches.py _stamina)
    Site('stamina_store', 'code', '1e204008 94000000/fc000000 bd00a668 f9400ff3 a940fbfd fc4207e8 d65f03c0', 2),
    # Link's Move action loading what a sprint costs (tools/patches.py _sprint_cost; Cheats > Free sprint): EnergyDash
    # (x19 + 0x20), the cost of a second of sprinting, and EnergyDashTrig (x19 + 0x30), the cost of a sprint press
    Site('sprint_cost', 'code', 'f9401268 2a1f03e1 bd400100 94000000/fc000000 f9400668 52800181 f94be900 '
         '94000000/fc000000', 2),
    Site('sprint_press_cost', 'code', 'f9401a68 bd400100 94000000/fc000000 f9400660 90000001/9f00001f '
         '91000021/ffc003ff 94000000/fc000000 36000000/fff8001f', 1),
    # the three battle music controllers taking the game's battle info, its first word the count their music starts on
    # (1 or more) and fades out on (0): the field battle music's and two others' (tools/patches.py _battle_info;
    # Cheats > Free sprint). Whichever music mode is on hands the info to one of them.
    Site('battle_info_field', 'code', 'a9402428 a91da408 d65f03c0', 0),
    Site('battle_info_b', 'code', 'a9402428 a91da408 d3609d28 34000008/ff00001f 52800048 b901e808 d65f03c0 '
         'b941dc08', 0),
    Site('battle_info_c', 'code', 'b9400028 b901d008 d65f03c0', 0),
    # the horse's crash check loading its gear, and the entry of its stop check (tools/patches.py _horse_crash,
    # _horse_stop_check; Cheats > Improved horse controls)
    Site('horse_crash', 'code', 'b9403428 7100111f 54000001/ff00001f d10303ff 6d072beb f90053f5 6d0823e9 a9097bfd', 0),
    Site('horse_stop_check', 'code', 'b940382a 3400000a/ff00001f f9400029 b9400928 37100008/fff8001f 3947102b '
         '2a1f03e8 3500000b/ff00001f', 0),
    # Gameplay > Keep crouch after camera (tools/patches.py _camera_*; runtime/camera_stealth.c): crouch idle
    # target in the Player AI selector, Camera entry, the two Camera -> Wait exit branches, and Camera::leave().
    Site('camera_squat_idle', 'code',
         'f9400008 f9431d08 d63f0100 36000000/fff8001f 90000001/9f00001f 91000021/ffc003ff 14000000/fc000000 '
         'aa1303e0 94000000/fc000000 37000000/fff8001f', 4),
    Site('camera_enter_start', 'code',
         'f9400668 90000001/9f00001f 91000021/ffc003ff aa1303e0 b94c4109 121e7929 b90c4109 94000000/fc000000', 0),
    Site('camera_exit_finished', 'code',
         '90000008/9f00001f 91000108/ffc003ff 91004335 910483e1 a91223f5 aa1303e0 94000000/fc000000 37000000/fff8001f '
         '90000008/9f00001f 91000108/ffc003ff 910023e1 aa1303e0 a900a3f5 94000000/fc000000 37000000/fff8001f '
         '90000008/9f00001f 91000108/ffc003ff 91004335 910443e1 a91123f5 aa1303e0 94000000/fc000000 37000000/fff8001f '
         '90000008/9f00001f 91000108/ffc003ff 910403e1 aa1303e0 a91023f5 94000000/fc000000 37000000/fff8001f '
         '90000008/9f00001f 91000108/ffc003ff 91004335 9103c3e1', 7),
    Site('camera_exit_active', 'code',
         '36000000/fff8001f 14000000/fc000000 f9400268 aa1303e0 f9406908 d63f0100 f9400008 f9401108', 1),
    Site('camera_leave', 'code',
         '94000000/fc000000 14000000/fc000000 90000008/9f00001f 91000108/ffc003ff f9400100 14000000/fc000000 '
         'a9be7bfd f9000bf3 910003fd aa0003f3 f9400400 94000000/fc000000 37000000/fff8001f 94000000/fc000000', 4),
    # the ridden horse's update after its prologue (x0: the riding action), its steering loading the gear's stick
    # scale, and its steering storing the horse's play rate (tools/patches.py _horse_ride, _horse_stick_scale,
    # _horse_play_rate; runtime/improved_horse.c; Cheats > Improved horse controls)
    Site('horse_ride', 'code', 'd10a03ff aa0003f3 52804702 910123e0 2a1f03e1 910123f6 94000000/fc000000 f9400660', 0),
    Site('horse_stick_scale', 'code', 'bc697901 1e210800 bc1b43a0 94000000/fc000000 d10153a1 aa1403e0 aa1303e2 '
         '94000000/fc000000', 0),
    Site('horse_play_rate', 'code', 'bd015500 14000000/fc000000 3948b669 1e2e2140 3948ba68 54000004/ff00001f '
         '35000009/ff00001f 34000008/ff00001f', 0),
    # the pouch taking arrows off for the player's bow (tools/patches.py _remove_arrows; Accessibility > Infinite arrows)
    Site('pouch_remove_arrows', 'code', 'd101c3ff a9017bfd a9026ffc a90367fa a9045ff8 a90557f6 a9064ff4 910043fd '
         '90000008/9f00001f aa0003f3 f9400108/ffc003ff aa0103e0', 0),
    # PC VR (runtime/pc_vr.c, runtime/hle/pc_vr_head_look.c, pc_vr_projection_trace.c, labo_camera_trace.c,
    # labo_sensor_trace.c), one optional group: without all of them PC VR stays off.
    # The camera matrix builder the head look composes into, and the return addresses of its base, layer and eye calls;
    # the projection builder and device and their callers; a render layer's begin (its end, which no code calls
    # directly, follows it at +0x58: runtime/hle/pc_vr_projection_trace.c); the Labo VR object's switch, the two managers the
    # Options screen notifies after it, and the transition calls PC VR redirects; the sensor and mask call returns;
    # the eight camera types' update entries (their internal call sites keep their offsets); the camera classes'
    # vtables; the Labo manager's slot and the UI root (automation)
    Site('pcvr_matrix_builder', 'code', '2d470801 2d480c00 1e232020 2d491404 1e240440 1e250400 54000000/ff00001f '
         '1e233823', 0),
    Site('pcvr_base_caller', 'code', 'f8408428 f9401108 d63f0100 aa1303e0 f9400bf3 a8c27bfd 14000000/fc000000', 3),
    Site('pcvr_layer_base_caller', 'code', '14000000/fc000000 f94102c8 bd437900 bd437d01 bd437102 bd437503 1e222800 '
         '1e232821', 0),
    Site('pcvr_eye_caller', 'code', 'f9010ed8 14000000/fc000000 3600001a/fff8001f 79401308 910d22d9 7906a2c8 3cc0c300 '
         '3d803140', 0),
    Site('pcvr_proj_builder', 'code', 'bd409800 1e2c1003 52b7f008 2d558801 1e202800 2d569404 b900043f f800c03f', 0),
    Site('pcvr_proj_device', 'code', '3dc00042 51000468 7100111f 3d800022 3dc00444 3d800424 3dc00840 3d800820', 0),
    Site('pcvr_proj_build_caller', 'code', '52802008 79001268 f9400268 91013261 91003262 b9408e63 aa1303e0 f9403108', 0),
    Site('pcvr_proj_device_caller', 'code', '3900267f f9400bf3 a8c27bfd d65f03c0 39402668 35000008/ff00001f '
         '14000000/fc000000', 0),
    Site('pcvr_proj_root_return', 'code', '910006e8 92407d17 6b08027f 54000001/ff00001f b944ca88 34000008/ff00001f '
         'b9403297 34000017/ff00001f', 0),
    Site('pcvr_proj_sibling_return', 'code', '14000000/fc000000 f94102d7 910f22e0 f941e6e8 f9400d08 d63f0100 f94102c8 '
         '90000009/9f00001f', 0),
    Site('pcvr_render_begin', 'code', 'a9be7bfd f9000bf3 910003fd f9400008 f9402433 b9400021 f9403d08 d63f0100', 0),
    Site('pcvr_render_begin_caller', 'code', '52810408 9b2856c9 b9800928 34000008/ff00001f f9400935 d37df116 f9400288 '
         'aa1403e0', 0),
    Site('labo_set_vr', 'code', '52800028 390c0001 390c0408 d65f03c0', 0),
    Site('labo_end_vr_call', 'code', 'd65f03c0 94000000/fc000000 94000000/fc000000 a8c17bfd 14000000/fc000000', 2),
    Site('labo_enable_rec_call', 'code', 'd65f03c0 94000000/fc000000 94000000/fc000000 a8c17bfd 14000000/fc000000', 3),
    Site('labo_vr_notify_1', 'code', 'a9bd7bfd a90157f6 a9024ff4 910003fd 90000008/9f00001f 2a1f03f5 '
         'f9400108/ffc003ff 12000013 f9400116 b9402ac8 6b15011f f9401ac9 1a9f82a8 f8685934 94000000/fc000000 '
         'b4000014/ff00001f f9400288 aa0003e1 aa1403e0 f9400908 d63f0100 36000000/fff8001f 710076bf', 0),
    Site('labo_vr_notify_2', 'code', 'a9bd7bfd a90157f6 a9024ff4 910003fd 90000008/9f00001f 2a1f03f5 '
         'f9400108/ffc003ff 12000013 f9400116 b9402ac8 6b15011f f9401ac9 1a9f82a8 f8685934 94000000/fc000000 '
         'b4000014/ff00001f f9400288 aa0003e1 aa1403e0 f9400908 d63f0100 36000000/fff8001f f9400288', 0),
    Site('labo_sensor_ret', 'code', '7100041f b9000280 5400000b/ff00001f 2d4607e0 f9000e7f 2d470fe2 b900227f 2d434bf1', 0),
    Site('labo_track_start_ret', 'code', '394002a8 121f1914 f941c260 71000a9f 1a9f17e1 94000000/fc000000 71000a9f '
         '54000001/ff00001f', 0),
    Site('pcvr_cam_update_0', 'code', 'd104c3ff 6d0a3bef 6d0b33ed 6d0c2beb 6d0d23e9 a90e7bfd a90f67fc a9105ff8 '
         'a91157f6 a9124ff4 910383fd 91008015', 0),
    Site('pcvr_cam_update_1', 'code', 'd104c3ff 6d093bef f90073fc 6d0a33ed 6d0b2beb 6d0c23e9 a90d7bfd a90f67fa', 0),
    Site('pcvr_cam_update_2', 'code', 'd10203ff 6d032beb 6d0423e9 a9057bfd a90657f6 a9074ff4 910143fd aa0003f3', 0),
    Site('pcvr_cam_update_3', 'code', 'd10343ff 6d043bef 6d0533ed 6d062beb 6d0723e9 a9087bfd a90967fa a90a5ff8 '
         'a90b57f6 a90c4ff4 910203fd 91008015 aa0003f3 aa1503e0 94000000/fc000000 b4000000/ff00001f aa0003f4 '
         '94000000/fc000000 b5000000/ff00001f 6d4723e9 6d462beb 6d4533ed 6d443bef a94c4ff4 a94b57f6 a94a5ff8 a94967fa '
         'a9487bfd 910343ff d65f03c0 bd486280 aa0003f6 910093e0 bd486e81 9100c3e1 1e213800 bd486682 bd487283 bd486a84 '
         'bd487685 1e233841 2d0607e0 1e253880 bd003be0 94000000/fc000000 f84243e8 910043e0 a90123ff b9402fe8 b90023e8 '
         '94000000/fc000000 94000000/fc000000 1e20400a', 0),
    Site('pcvr_cam_update_4', 'code', 'd10343ff 6d043bef 6d0533ed 6d062beb 6d0723e9 a9087bfd a90967fa a90a5ff8 '
         'a90b57f6 a90c4ff4 910203fd 91008015 aa0003f3 aa1503e0 94000000/fc000000 b4000000/ff00001f aa0003f4 '
         '94000000/fc000000 b5000000/ff00001f 6d4723e9 6d462beb 6d4533ed 6d443bef a94c4ff4 a94b57f6 a94a5ff8 a94967fa '
         'a9487bfd 910343ff d65f03c0 bd486280 aa0003f6 910093e0 bd486e81 9100c3e1 1e213800 bd486682 bd487283 bd486a84 '
         'bd487685 1e233841 2d0607e0 1e253880 bd003be0 94000000/fc000000 f84243e8 910043e0 a90123ff b9402fe8 b90023e8 '
         '94000000/fc000000 94000000/fc000000 1e204009', 0),
    Site('pcvr_cam_update_5', 'code', 'd10243ff 6d023bef 6d0333ed 6d042beb 6d0523e9 a9067bfd a90757f6 a9084ff4 '
         '910183fd 91008015', 0),
    Site('pcvr_cam_update_6', 'code', 'd10243ff fd0013ec 6d032beb 6d0423e9 a9057bfd a9065ff8 a90757f6 a9084ff4 '
         '910143fd 91008015', 0),
    Site('pcvr_cam_update_7', 'code', 'd102c3ff fd002bea 6d0623e9 a9077bfd a9085ff8 a90957f6 a90a4ff4 9101c3fd '
         'aa0003f3', 0),
    Site('pcvr_actor_vtable', 'ref', '90000008/9f00001f 91000108/ffc003ff f9000268 91152109 9115c10a 91166108 '
         'f9042268 52a7f008', 1),
    Site('pcvr_eye_vtable', 'ref', '90000008/9f00001f 91000108/ffc003ff f9000268 b9400ac8 f94002c9 f9001e69 b9004268 '
         'f94002a8', 1),
    Site('pcvr_proj_vtable', 'ref', '90000009/9f00001f 91000129/ffc003ff f80b401f b9400108 f9000009 bd009400 b9008c08 '
         'd2906bc8', 1),
    Site('pcvr_base_vtable', 'ref', '90000009/9f00001f 91000129/ffc003ff b9004268 52a7f008 f9000269 f8054268 aa1303e8 '
         '9100e129', 1),
    Site('labo_manager_slot', 'ref', '90000008/9f00001f 52829989 f9400108/ffc003ff f9400108 f941bd08 f9400d08 '
         '8b090108 885f7d09', 2),
    Site('automation_screen_root', 'ref', '90000008/9f00001f aa0003f3 f9400108/ffc003ff f9400108 b9402909 f9401908 '
         '7101393f 52804e09 9a9f8129 f8696914 94000000/fc000000 b4000014/ff00001f f9400288 aa0003e1 aa1403e0 f9400908 '
         'd63f0100 36000000/fff8001f aa1403e0 94000000/fc000000 37000000/fff8001f aa1403e0', 2),
    # PC VR's world-tied HUD (tools/patches.py _hud_point; runtime/pc_vr_hud.c): the HUD's world-to-screen helper's last
    # call, to the Labo VR object's adjustment of the position it writes
    Site('pcvr_hud_point', 'code', 'f941c100 94000000/fc000000 fd4013e8 a9454ff4 a94457f6 a9437bfd 910183ff d65f03c0', 1),
    # PC VR's eye masks from the headset (tools/patches.py _lens_draw; runtime/nvn/vulkan/vr_lens_mask.h): the eye
    # scenes' screen-quad draw. While the Labo display's mask flag is set (the graphics object + 0x35d8), it draws the
    # lens-shaped 48-index polygon (the object + 0x858) instead of the full-screen triangle (+ 0x478): the bl after
    # that choice and the load of its index count
    Site('pcvr_lens_draw', 'code', '38696909 9121610a 9111e108 7100013f 9a8a0100 b9400c02 94000000/fc000000 6d5223e9',
         6),
    # ... and Labo's depth mask, drawn in each eye before its scene while the flag is set: the area outside the lens
    # (the object + 0x8b8, its index count at + 0x8c4) at the nearest depth, so the scene's geometry skips it
    Site('pcvr_lens_depth_draw', 'code', 'f94002a8 aa1303e1 52800023 b948c502 9122e100 94000000/fc000000 17ffffca '
         'a9be7bfd', 5),
    # PC VR's HUD by screen (tools/patches.py _screen_draw; runtime/pc_vr_hud.c): the UI screen manager's draw loop calls
    # each screen of a draw unit through its vtable (+0x58): the blr, with x0 the screen and w22 its id
    Site('pcvr_screen_draw', 'code', 'd63f0100 910006e8 110006d6 92407d17 6b0802bf 54000001/ff00001f a9444ff4 f9400bf9', 0),
    # nn::ui2d::Pane::Draw (tools/patches.py _pane_draw): its call of the pane's own DrawSelf (vtable +0xb0), x0 the pane
    Site('pcvr_pane_draw', 'code', 'f94002a8 aa1503e0 aa1403e1 aa1303e2 f9405908 d63f0100 394162a8 39416aa9', 5),
    # ... and its call of each child's Draw (vtable +0xa8), x0 the child: panes that override Draw (capture panes, a
    # speech balloon's background) draw without going through DrawSelf
    Site('pcvr_pane_child_draw', 'code', 'f85f8ea8 aa1403e1 aa1503e0 aa1303e2 f9405508 d63f0100 f9400ab5 eb1602bf', 5),
    # A speech balloon's positioning (tools/patches.py _balloon_point): the store of its pane's x, after projecting the NPC's
    # head with the same camera calls as the HUD's helper (the camera-space point at sp + 0x1c)
    Site('pcvr_balloon_point', 'code', 'f9400a68 aa1403e0 bd003100 39416109 bd003508 b900391f 321c0129 39016109', 2),
    # PC VR's markers by the game's own store (tools/patches.py _marker_store; runtime/pc_vr_hud.c, the marker links),
    # all in the world screen's own code. Eight callers of the HUD's world-to-screen helper copy its result into a pane
    # themselves, right after the call: the store of the pane's +0x38 (the pane in x20, x21 or x22), between the load
    # of the result from the caller's stack and the stores of the pane's translate
    Site('pcvr_marker_store_1', 'code', 'b9003abf 394162a8 321c0108 bd0036a1 bd0032a0 390162a8 f9400368 '
         '14000000/fc000000', 0),
    Site('pcvr_marker_store_2', 'code', 'b9003a9f 39416288 3200e108 2d060680 39016288 14000000/fc000000 aa1f03e0 '
         'f94007e9', 0),
    Site('pcvr_marker_store_3', 'code', 'b9003a9f 39416288 321c0108 2d060680 39016288 aa1303e0 94000000/fc000000 '
         '14000000/fc000000 f95f6e68', 0),
    Site('pcvr_marker_store_4', 'code', 'b9003a9f 39416288 321c0108 2d060680 39016288 aa1303e0 94000000/fc000000 '
         '14000000/fc000000 2f00e400', 0),
    Site('pcvr_marker_store_5', 'code', 'b9003abf 394162a8 321c0108 2d0606a0 390162a8 f95f6e68 f9401108 f9400d08', 0),
    Site('pcvr_marker_store_6', 'code', 'b9003a9f 39416288 321c0108 bd003681 bd003280 39016288 f95f6e68 f9401108', 0),
    Site('pcvr_marker_store_7', 'code', 'b9003adf 394162c8 321c0108 2d0606c0 390162c8 14000000/fc000000 '
         '34000016/ff00001f 14000000/fc000000', 0),
    Site('pcvr_marker_store_8', 'code', 'b9003a9f 39416288 321c0108 bd003681 bd003280 39016288 f95f9a68 f9401108', 0),
    # ... the caller that hands the result to a component's set-position function (which writes the pane at
    # [[component + 8] + 0x18]): that call, x0 the component and x1 the helper's output buffer
    Site('pcvr_marker_component_call', 'code', '94000000/fc000000 f960de88 2d7e87a0 bc5fc3a2 2d451103 1e240826 '
         '1f001860 2d460d01', 0),
    # ... and a second wheel-like piece (its component in x20), whose flag in w25 says whether it follows Link at the
    # helper's result (its output buffer at sp + 0x18) or is given a fixed HUD place: the flag's two assignments
    Site('pcvr_marker_wheel_follow', 'code', '2a1f03f9 52801ff6 14000000/fc000000', 0),
    Site('pcvr_marker_wheel_flat', 'code', '52800039 aa1803f7 0f056682 bc442ee0 1e283801 1e222020 5400000b/ff00001f '
         '0f016682', 0),
    # The rune target test's setup (tools/patches.py _rune_test; D078): its call of the Labo VR adjustment of the screen
    # position it tests (x21 the test's entry, its constants block's address at +0x10; the position at x29 - 0x18)
    # The UI's screen parallax (tools/patches.py _ui_parallax): the setter that stores it in a view and the views chained
    # to it (+0x404), called by the Labo display's setup for its three UI views each frame
    Site('pcvr_ui_parallax', 'code', 'bd040400 f9410400 b5000000/ff00001f d65f03c0', 0),
    # PC VR's eye images by the game's own code (tools/patches.py _layer_draw; runtime/hle/pc_vr_render_trace.h). Every
    # render layer is finished the same way: one function draws the layer's HDR scene into its finished image (and
    # Labo's mask over it), another draws that image into its place in the final image. Each does it with one call of
    # the screen-quad draw: these two calls
    Site('pcvr_layer_final_draw', 'code', '94000000/fc000000 52a85408 f941c2c0 aa1503e1 1e270100 aa1403e2 aa1803e3 '
         'aa1303e4', 0),
    Site('pcvr_layer_placement_draw', 'code', '94000000/fc000000 aa1803fa b400001a/ff00001f 90000008/9f00001f aa1a03e1 '
         'f9400108/ffc003ff f9400100 94000000/fc000000', 0),
    # ... and the frame's display step, which draws the final image to the display: through the callback a display
    # registers (Labo's lens pass; the blr x8) or, when there is none, with a plain copy (the bl)
    Site('pcvr_display_lens_draw', 'code', 'd63f0100 910023e8 9101a100 94000000/fc000000 910023e0 94000000/fc000000 '
         '910603e0 94000000/fc000000', 0),
    Site('pcvr_display_copy_draw', 'code', '910563e2 94000000/fc000000 14000000/fc000000', 1),
    Site('pcvr_rune_test', 'code', '94000000/fc000000 2d7d07a0 2d0227ea 910023e3 aa1503e0 2a1f03e2 2a1f03e4 52800025', 0),
    # ... and the call of the test's draw function (1.9.0 0x700126fb68: x0 the manager, x1 the draw context, x2 the
    # view), from its only caller 0x700126aa78: the test's screen-quad draws are recorded during it
    Site('pcvr_rune_test_draw', 'code', '94000000/fc000000 aa1303e0 aa1403e1 2a1f03e2 94000000/fc000000 14000000/fc000000',
         0),
    # PC VR's effect draws (tools/patches.py _effect_draw; runtime/hle/pc_vr_render_trace.h PC_VR_LAYER_DRAW_EFFECT): the
    # game's effect draw for a view (1.9.0 0x700105cd88, called from the render layers' draw steps) sets nn::vfx's view
    # and draw groups up, then calls nn::vfx's System::Draw (0x70000663b0: x0 the system, w1 the processing index, x2
    # the command buffer, x4 the view): that call, the only one in the module. Every particle and stripe nn::vfx draws
    # (its emitters' draws, 0x7000066584, and the game's draw callback it calls back, 0x700105d1c0) is recorded during it
    Site('pcvr_effect_draw', 'code', '94000000/fc000000 f9401668 b9400909 6b16013f 5400000d/ff00001f f9400908 52810609 '
         '9b2922c8', 0),
    # PC VR's head view for the game's visibility decisions (tools/patches.py _cull_matrix, _cull_fov;
    # runtime/hle/pc_vr_cull_view.c; 1.9.0 only). The game builds a visibility view from a camera's matrix (the camera
    # + 8) and its field of view after Labo VR's change of it (1.9.0 0x700126138c: s0 fovy, s1 aspect, x1 and x2
    # where the changed pair goes), in three places:
    # ... Labo's change itself, found on its own: the patches check their calls go there
    Site('pcvr_labo_fov', 'code', '6dbd23e9 a9017bfd a9024ff4 910043fd 1e204028 1e2c1001 aa0203f3 aa0103f4 1e210800 '
         '1e210909 94000000/fc000000 1e280800 1e291800 94000000/fc000000 1e202800', 0),
    # ... the actor level-of-detail manager's update (1.9.0 0x70010bb1c8): `add x20, x0, #8`, the matrix its two view
    # volumes are built from (the camera in x0, kept in x21), and its call of Labo's change
    Site('pcvr_lod_cull_matrix', 'code', 'f94002a0 94000000/fc000000 b4000000/ff00001f aa1403e2 91002014 aa0003f5 '
         '910e43e0 aa1403e1 94000000/fc000000 90000008/9f00001f d10013a1 f9400108/ffc003ff bd44bbe0 d10023a2 bd44bfe1 '
         'f9400108/ffc003ff f9400100/ffc003ff 94000000/fc000000', 4),
    Site('pcvr_lod_cull_fov', 'code', 'f94002a0 94000000/fc000000 b4000000/ff00001f aa1403e2 91002014 aa0003f5 '
         '910e43e0 aa1403e1 94000000/fc000000 90000008/9f00001f d10013a1 f9400108/ffc003ff bd44bbe0 d10023a2 bd44bfe1 '
         'f9400108/ffc003ff f9400100/ffc003ff 94000000/fc000000', 17),
    # ... the visibility test (1.9.0 0x70010ec544, the camera kept in x20): its call of Labo's change and
    # `add x1, x20, #8`, the matrix for its frustum. Its twin for the camera manager's second set (0x70010ec624) has
    # the same words, so each pattern starts with the two words before its function: the end of the point wrapper
    # (movi d0, #0; b) for the test, the twin's own wrapper (ldp s1, s2, [x0, #0x6c]; b) for the twin
    Site('pcvr_vis_cull_fov', 'code', '2f00e400 14000000/fc000000 d10243ff fd002bea 6d0623e9 a9077bfd a9084ff4 9101c3fd '
         '90000008/9f00001f 1e20402a 1e204008 bd407400 bd400101/ffc003ff 1e212000 5400000a/ff00001f 90000008/9f00001f '
         'aa0003f4 1e204049 f9400108/ffc003ff aa0103f3 f9400100/ffc003ff 94000000/fc000000 f9400008 f9400108/ffc003ff '
         'd63f0100 90000008/9f00001f 1e204001 d10053a1 f9400108/ffc003ff bd407680 d10063a2 f9400108/ffc003ff '
         'f9400100/ffc003ff 94000000/fc000000 910023e0 94000000/fc000000 2d7d03a1 1e204142 1e204123 91002281', 33),
    Site('pcvr_vis_cull_matrix', 'code', '2f00e400 14000000/fc000000 d10243ff fd002bea 6d0623e9 a9077bfd a9084ff4 '
         '9101c3fd 90000008/9f00001f 1e20402a 1e204008 bd407400 bd400101/ffc003ff 1e212000 5400000a/ff00001f '
         '90000008/9f00001f aa0003f4 1e204049 f9400108/ffc003ff aa0103f3 f9400100/ffc003ff 94000000/fc000000 f9400008 '
         'f9400108/ffc003ff d63f0100 90000008/9f00001f 1e204001 d10053a1 f9400108/ffc003ff bd407680 d10063a2 '
         'f9400108/ffc003ff f9400100/ffc003ff 94000000/fc000000 910023e0 94000000/fc000000 2d7d03a1 1e204142 1e204123 '
         '91002281', 39),
    Site('pcvr_vis2_cull_fov', 'code', '2d4d8801 14000000/fc000000 d10243ff fd002bea 6d0623e9 a9077bfd a9084ff4 9101c3fd '
         '90000008/9f00001f 1e20402a 1e204008 bd407400 bd400101/ffc003ff 1e212000 5400000a/ff00001f 90000008/9f00001f '
         'aa0003f4 1e204049 f9400108/ffc003ff aa0103f3 f9400100/ffc003ff 94000000/fc000000 f9400008 f9400108/ffc003ff '
         'd63f0100 90000008/9f00001f 1e204001 d10053a1 f9400108/ffc003ff bd407680 d10063a2 f9400108/ffc003ff '
         'f9400100/ffc003ff 94000000/fc000000 910023e0 94000000/fc000000 2d7d03a1 1e204142 1e204123 91002281', 33),
    Site('pcvr_vis2_cull_matrix', 'code', '2d4d8801 14000000/fc000000 d10243ff fd002bea 6d0623e9 a9077bfd a9084ff4 '
         '9101c3fd 90000008/9f00001f 1e20402a 1e204008 bd407400 bd400101/ffc003ff 1e212000 5400000a/ff00001f '
         '90000008/9f00001f aa0003f4 1e204049 f9400108/ffc003ff aa0103f3 f9400100/ffc003ff 94000000/fc000000 f9400008 '
         'f9400108/ffc003ff d63f0100 90000008/9f00001f 1e204001 d10053a1 f9400108/ffc003ff bd407680 d10063a2 '
         'f9400108/ffc003ff f9400100/ffc003ff 94000000/fc000000 910023e0 94000000/fc000000 2d7d03a1 1e204142 1e204123 '
         '91002281', 39),
    # ... the grass (tools/patches.py _grass_volume, _grass_matrix): the grass view object's update (1.9.0
    # 0x700149056c) loading the layer record's culling volume, projection matrix and view matrix for the grass
    Site('pcvr_grass_volume', 'code', '39066128 39402688 8b080e68 f9400d08 f9000128 bd403d00 1e202000', 3),
    Site('pcvr_grass_projection', 'code', '3500001a/ff00001f f9430a68 9b385789 3dc00100 3d800920 3dc00500', 1),
    Site('pcvr_grass_view', 'code', '3dc00d00 3d801520 f9430668 3dc00100 3d801920 3dc00500', 2),
    # ... and the game's own view volume functions the runtime builds the grass's volume with, from the grass slot's
    # builder (1.9.0 0x7001493110): a volume set from a view matrix (0x7000f8f780) then from a projection (0x7000f90cf4),
    # and the volume copy (0x700101a468; an identical copy elsewhere makes its own pattern ambiguous)
    Site('pcvr_volume_view', 'call', '52806a08 910022a1 9b28633b 9100f377 aa1703e0 94000000/fc000000 aa1703e0 aa1603e1 '
         '94000000/fc000000 3cc3c360', 5),
    Site('pcvr_volume_projection', 'call', '52806a08 910022a1 9b28633b 9100f377 aa1703e0 94000000/fc000000 aa1703e0 '
         'aa1603e1 94000000/fc000000 3cc3c360', 8),
    Site('pcvr_volume_copy', 'call', '52806a08 aa0303e1 9b286336 aa0303f5 9100f2c0 94000000/fc000000 3dc002a0 910c62c8 '
         '3d800100', 5),
    # ... and a model's level of detail by its distance (tools/patches.py _model_detail_depth): the routine every
    # builder of a layer's model entries calls to pick the level (1.9.0 0x70010575a4), at its load of the entry's
    # stored depth
    Site('pcvr_model_detail_depth', 'code', '7100082a 5400022b bd401801 bd400042 1e222821 1e202020', 2),
    # sead::LookAtCamera::doUpdateMatrix (tools/patches.py _lookat_update): its first load of the eye (+0x38); eye,
    # at (+0x44) and up (+0x50) make the view matrix written to x1 (free camera, runtime/hle/freecam.c)
    Site('lookat_update', 'code', '2d470801 2d480c00 1e232020 2d491404 1e240440 1e250400 54000000/ff00001f 1e233823', 0),
    # Native quick-selector sorting (runtime/quick_inventory_sort.c), optional as a complete group.
    Site('quick_sort_enter', 'code', 'fc1d0fe8 a900fbfd f9000ff5 a9024ff4 910023fd b978e014 aa0003f3 7100069f', 0),
    Site('quick_sort_menu', 'code', 'a9be7bfd a9014ff4 910003fd 52871b08 aa0003f3 8b080014 f95bd400 b4000000/ff00001f', 0),
    Site('pouch_page_count', 'code', '2a0103e9 7100c83f 5288460a 72a0008a 9a9f3129 8b090c09 f86a692c b9407809', 0),
    Site('pouch_page_item', 'code', '7100c43f 5400000d/ff00001f aa1f03e0 d65f03c0 a9bd7bfd f9000bf5 a9024ff4 910003fd 2a0203f3 '
         '2a0103f5 aa0003f4 94000000/fc000000 2a0003e8 2a1503e9 7100cabf aa1f03e0 9a9f3129 6b13011f '
         '5400000d/ff00001f 8b090e88 52884609 72a00089 f869690b', 0),
    Site('pouch_item_type', 'code', 'd101c3ff a9037bfd f90023f7 a90557f6 a9064ff4 9100c3fd 90000016/9f00001f 90000008/9f00001f', 0),
    Site('quick_sort_modal', 'code', '90000008/9f00001f 91000108/ffc003ff f9400108 b9402909 f9401908 71002d3f 52800b09 9a9f8129 '
         'f8696900 b4000000/ff00001f 14000000/fc000000 d65f03c0', 0),
    Site('pouch_sort', 'code', 'd10103ff a9017bfd a90257f6 a9034ff4 910043fd 9100a013 aa0003f4 2a0203f5', 0),
    Site('quick_sort_refill', 'code', 'a9be7bfd a9014ff4 910003fd aa0003f3 71000c3f 54000008/ff00001f 2a0103e8 90000009/9f00001f', 0),
    Site('quick_sort_mapping', 'code', 'a9be7bfd a9014ff4 910003fd b9773814 aa0003f3 7100069f 54000000/ff00001f 35000014/ff00001f', 0),
    Site('quick_sort_cursor', 'code', 'a9bd7bfd f9000bf5 a9024ff4 910003fd 2a0203f3 2a0103f4 2a0203e1 2a1f03e2', 0),
    Site('quick_sort_labels', 'code', 'd10483ff a90e7bfd f9007bfc a91057f6 a9114ff4 910383fd aa0003f3 d10043a0', 0),
    Site('pouch_lock', 'call', '94000000/fc000000 f9401be9 f9400fa0 14000000/fc000000 aa1403e0 aa0903f4 94000000/fc000000 '
         'aa1403e9', 0),
    Site('pouch_unlock', 'call', '94000000/fc000000 aa1403e9 14000000/fc000000 f9000fa0 aa0803e0 52800601 52800082 d63f0120', 0),
    Site('quick_sort_event', 'call', '94000000/fc000000 37000000/fff8001f 2a1f03e0 94000000/fc000000 36000000/fff8001f 2a1f03e0 '
         '2a1f03e1 94000000/fc000000 aa1303e0 aa1403e1', 0),
    Site('quick_sort_player_busy', 'call', '94000000/fc000000 2a0003f4 d10183a0 94000000/fc000000 90000015/9f00001f 90000008/9f00001f '
         '91000108/ffc003ff 14000000/fc000000', 0),
    # Native quick-selector drop and asynchronous equipped-item completion.
    Site('quick_drop_tick', 'code', 'd10183ff a9027bfd a9035ff8 a90457f6 a9054ff4 910083fd 91419008 aa0003f3', 0),
    Site('quick_drop_actor_call', 'code', 'd63f0100 aa1503e0 94000000/fc000000 a9424ff4 52800020 f9400bf5 a8c37bfd d65f03c0', 0),
    Site('player_drop_forbidden', 'code', 'a9be7bfd f9000bf3 910003fd aa0003f3 94000000/fc000000 36000000/fff8001f 2a1f03e0 '
         '14000000/fc000000', 0),
    Site('actor_info_lookup', 'code', 'd10103ff a9017bfd a90257f6 a9034ff4 910043fd aa0003f6 aa0203e0 2a0303f5', 0),
    Site('actor_info_has_tag', 'code', 'd10203ff a9047bfd a9055ff8 a90657f6 a9074ff4 910103fd aa0103f4 f9400421', 0),
    Site('pouch_discard', 'code', 'd10183ff a9027bfd a9035ff8 a90457f6 a9054ff4 910083fd 9100a013 aa0003f4', 0),
    Site('player_equipment', 'code', 'a9be7bfd f9000bf3 910003fd f9400008 2a0103f3 f9418908 d63f0100 2a1303e1 94000000/fc000000', 0),
    Site('pouch_default_name', 'code', '90000009/9f00001f 90000008/9f00001f 91000108/ffc003ff f9400129/ffc003ff 71001c3f 8b215108 '
         '7a423824 9a891100', 0),
    Site('player_schedule_equip', 'code', 'd100c3ff a9017bfd a9024ff4 910043fd 12800003 910023e4 f90007ff aa0003f3', 0),
    Site('pouch_equipped_snapshot', 'code', 'a9bd7bfd f9000bf5 a9024ff4 910003fd 91411008 9100a013 aa0003f4 911f8115', 0),
    Site('actor_index_assign', 'code', 'd10503ff a9127bfd a9134ffc 910483fd 39403008 aa0103f3 34000008/ff00001f f9400008', 0),
    Site('actor_index_resolve', 'code', 'd10143ff a9017bfd a9025ff8 a90357f6 a9044ff4 910043fd b9400808 3100051f', 0),
    Site('actor_index_release', 'code', '39403008 35000008/ff00001f 12800008 b9000808 d65f03c0 f9400008 3900301f b4000008/ff00001f', 0),
    Site('actor_link_release', 'code', 'a9be7bfd f9000bf3 910003fd 39400008 aa0003f3 34000008/ff00001f f9400660 b4000000/ff00001f', 0),
    Site('actor_link_alive', 'code', 'b9400809 3100053f 54000000/ff00001f f9400008 b940410a 6b09015f 54000001/ff00001f 39403009', 0),
    Site('pouch_cleanup', 'code', 'a9bb7bfd a90167fa a9025ff8 a90357f6 a9044ff4 910003fd 9100a013 aa0003f4', 0),
    Site('player_actor', 'call', '94000000/fc000000 b4000000/ff00001f 91316015 aa0003f4 aa1503e0 94000000/fc000000 b94c9a88 '
         'aa1503e0 321b0108 b90c9a88 94000000/fc000000 aa1303e0', 0),
    Site('quick_drop_trash_prohibited', 'call', '94000000/fc000000 36000000/fff8001f 52800037 14000000/fc000000 1e229000 d63f0100 d2800ba9 '
         '5286db88', 0),
    Site('quick_drop_actor_method', 'code', 'f942cc08 b4000008/ff00001f 79404d09 32000129 79004d09 14000000/fc000000 f942cc08 '
         'b4000008/ff00001f', 0),
    Site('actor_info', 'ref', '90000008/9f00001f 91000108/ffc003ff f9400117 b4000017/ff00001f f94006a8 3940011b '
         '3400001b/ff00001f f94002a8', 1),
    # Add items (tools/patches.py _item_tick; runtime/items.c): the entry of Link's damage handling (every frame in
    # Adding items (tools/patches.py _item_tick; runtime/items.c): the entry of Link's damage handling (every frame in
    # play: the game thread's place for the item get), the pause menu data's item get, and where the pause menu data's
    # address is (its load in the pickup code)
    Site('item_tick', 'code', 'd101c3ff fd000be8 a9027bfd a90367fa a9045ff8 a90557f6 a9064ff4 910083fd f9400c14', 0),
    Site('pouch_item_get', 'code', 'd10243ff a9037bfd a9046ffc a90567fa a9065ff8 a90757f6 a9084ff4 9100c3fd f90003e3', 0),
    Site('pouch_mgr', 'got', '90000017/9f00001f aa0003f5 f94002f7/ffc003ff 910003e0 f94002f6 94000000/fc000000 2a0003e2 '
         'aa1603e0', 2),
    # A treasure chest put in the world with an item in it (runtime/items.c items_request_chest), as the amiibo drop does
    # (its chest: TBox_Field_Iron with DropActor): the actor creation request (creator, name, heap, accessor, its
    # instance parameters, 0, 1), the creator and the object whose +0x28 is the heap it takes, and the instance
    # parameters' class, their setup, and how a raw (@P, @R: 12 bytes, type 4) and a string (DropActor) parameter go in
    Site('actor_create', 'call', '94000000/fc000000 14000000/fc000000 90000016/9f00001f f94002d6/ffc003ff f94002c0 '
         '94000000/fc000000 52a7f008 52a7f015', 0),
    Site('actor_creator', 'got', '90000008/9f00001f aa1403e0 f9400108/ffc003ff f9400115 f9400288 f9400d08 d63f0100 '
         '90000008/9f00001f 910103e4', 2),
    Site('actor_create_heap', 'got', '90000008/9f00001f 910103e4 f9400108/ffc003ff f9400681 aa1503e0 f9400108 f9401502 '
         'aa1303e3', 2),
    Site('inst_params_vt', 'got', '90000008/9f00001f f9400108/ffc003ff 91004108 a9047fe8 910103e8 91004100 f90007e0 '
         '94000000/fc000000', 1),
    Site('inst_params_init', 'call', '94000000/fc000000 f94006a8 9100c3e1 aa1503e0 b9003bff f9001bff bd43a509 bd43b50a', 0),
    Site('inst_params_add', 'call', '94000000/fc000000 90000008/9f00001f aa1403e0 f9400108/ffc003ff f9400115 f9400288 '
         'f9400d08 d63f0100 90000008/9f00001f 910103e4', 0),
    Site('inst_params_add_string', 'call', '94000000/fc000000 aa1403e0 94000000/fc000000 36000000/fff8001f 90000008/9f00001f '
         '91000108/ffc003ff 910043e1 a93aa3b5', 0),
    # Generic layout draw thunk and its native eligibility checks (runtime/ui_layout.c).
    Site('ui_layout_draw', 'code', '14000000/fc000000 f9013001 b4000001/ff00001f d10403ff a90a7bfd a90b6ffc a90c67fa a90d5ff8', 0),
    Site('ui_layout_native', 'code', '39440008 7100011f 39440809 3943f80a 3943fc0b 7a401920 7a400944 7a421964', 0),
    Site('ui_layout_pane', 'code', '39416008 52801029 6a28013f 54000001/ff00001f a9bd7bfd a90157f6 a9024ff4 910003fd', 0),
    # Optional native replacement lifecycle (verified 1.9.0).
    Site('replace_layout_calculate', 'code', 'f9400c08 b4000008/ff00001f d10183ff a9037bfd f90023f5 a9054ff4 9100c3fd aa0003f5 2a0203f4 910023e0', 0),
    Site('pouch_can_receive', 'code', 'a9be7bfd f9000bf3 910003fd aa0003f3 94000000/fc000000 35000000/ff00001f 90000008/9f00001f aa1303e1', 0),
    Site('replace_toast_start', 'code', 'a9bd7bfd a90157f6 a9024ff4 910003fd b9420008 7100011f f940fc09 9a9fc135', 0),
    Site('replace_pickup_status', 'code', 'd10143ff a9037bfd f90023f3 9100c3fd f9400408 b5000008/ff00001f 12800013 2a1303e0 f94023f3 a9437bfd 910143ff d65f03c0 aa0003e3', 0),
    Site('replace_draw_info_reset', 'code', 'a9bd7bfd f9000bf5 a9024ff4 910003fd f9408814 91042013 eb13029f 54000000/ff00001f', 0),
    Site('actor_link_copy', 'code', 'a9be7bfd a9014ff4 910003fd f9400434 aa0003f3 b5000014/ff00001f aa1f03f4 aa1303e0', 0),
    Site('replace_interaction_target', 'code', 'b5000001/ff00001f 2a1f03e0 d65f03c0 a9be7bfd f9000bf3 910003fd aa0103f3 2a1f03e1', 0),
    Site('actor_link_assign', 'code', 'a9be7bfd a9014ff4 910003fd aa0003f3 f9400400 aa0103f4 b4000000/ff00001f 39400268', 0),
    Site('replace_card_prepare', 'code', 'a9be7bfd a9014ff4 910003fd aa0103f3 aa0003f4 52800160 aa1f03e1 94000000/fc000000', 0),
    Site('replace_card_close', 'code', '90000008/9f00001f 91000108/ffc003ff f9400108 b9402909 f9401908 71002d3f 52800b09 9a9f8129 f8696900 b4000000/ff00001f f9400008 12800001', 0),
    Site('replace_component_update', 'code', 'a9be7bfd f9000bf3 910003fd 39442008 3904001f 34000008/ff00001f b9410408 aa0003f3', 0),
    Site('replace_ai_child_matches', 'code', 'a9be7bfd a9014ff4 910003fd f9400008 aa0103f3 f9406908 d63f0100 b4000000/ff00001f', 0),
    Site('replace_pickup_call_a', 'code', '94000000/fc000000 94000000/fc000000 910063e0 94000000/fc000000 d10043a0 94000000/fc000000 14000000/fc000000 90000001/9f00001f', 0),
    Site('replace_pickup_call_b', 'code', '94000000/fc000000 94000000/fc000000 910023e0 94000000/fc000000 d10043a0 94000000/fc000000 14000000/fc000000', 0),
    Site('replace_pickup_call_c', 'code', '94000000/fc000000 94000000/fc000000 910063e0 94000000/fc000000 910023e0 94000000/fc000000 14000000/fc000000 90000008/9f00001f', 0),
    Site('replace_safe_string_vtable_slot', 'ref', '90000008/9f00001f f9400108/ffc003ff 9000000a/9f00001f 9100014a/ffc003ff b909613f 90000009/9f00001f 91000129/ffc003ff aa1303e0', 1),
    Site('replace_string_assign', 'call', '910063e1 94000000/fc000000 f94017e8 14000000/fc000000', 1),
    Site('replace_card_manager_slot', 'ref', '90000008/9f00001f 2a0003e2 f9400108/ffc003ff f9400100 b4000000/ff00001f 7100045f 1a9fd7e1 94000000/fc000000', 2),
    Site('replace_pouch_manager_slot', 'ref', '90000008/9f00001f 0b170317 f9400108/ffc003ff f9400100 94000000/fc000000 90000004/9f00001f 910043e3 f9400084/ffc003ff', 2),
    Site('replace_interaction_manager_slot', 'ref', '90000017/9f00001f 910163e1 f94002f7/ffc003ff f94002e0 94000000/fc000000 37000000/fff8001f b943ae68 12147908', 2),
    Site('replace_chest_closed_wait_name', 'ref', 'd63f0100 90000001/9f00001f 91000021/ffc003ff aa1303e0 aa1f03e2 94000000/fc000000 f94013f3 a9417bfd', 2),
    Site('replace_weapon_modifier', 'call', '94000000/fc000000 90000017/9f00001f aa0003f5 f94002f7/ffc003ff 910003e0 f94002f6 94000000/fc000000 2a0003e2', 0),
    Site('replace_toast_vtable', 'ref', '90000008/9f00001f 91000108/ffc003ff 3905127f 52a7f009 f9000268 91052268 b9014269 12800009', 1),
    Site('replace_game_picture_vtable', 'ref', '14000000/fc000000 a9be7bfd f9000bf3 910003fd 90000008/9f00001f aa0203e4 f9400108/ffc003ff aa0303e5 aa0103e3 f9400108 aa1f03e1 aa0003f3 f9402502 94000000/fc000000 90000008/9f00001f 91000108/ffc003ff', 15),
    Site('replace_picture_vtable_base', 'got', '90000008/9f00001f aa1403f9 f9400108/ffc003ff 91004108 f80f8728 aa1903e0 94000000/fc000000 79408a68', 2),
    Site('replace_popup', 'code', 'd10103ff a9027bfd f9001bf3 910083fd b9400028 71001d1f 54000000/ff00001f 7100191f', 0),
    Site('replace_item_popup', 'code', 'f9400408 39400108 34000008/ff00001f d10103ff a9017bfd a90257f6 a9034ff4 910043fd', 0),
    Site('replace_receive', 'code', 'd106c3ff a9167bfd f900bbfc a9185ff8 a91957f6 a91a4ff4 910583fd 90000008/9f00001f', 0),
    Site('replace_card_close_gate', 'code', 'd100c3ff a9017bfd a9024ff4 910043fd aa0003f4 b940c000 2a0103f3 94000000/fc000000', 0),
    Site('replace_card_action', 'code', 'd10243ff a9067bfd f9003bf5 a9084ff4 910183fd f9400008 aa0003f3 f9401508', 0),
    Site('replace_chest_update', 'code', 'd101c3ff a9037bfd a9045ff8 a90557f6 a9064ff4 9100c3fd 90000017/9f00001f 90000008/9f00001f 91000108/ffc003ff f94002f7/ffc003ff 910023e1', 0),
    Site('replace_query', 'code', 'a9bf7bfd 910003fd f9401009 9100a008 aa0803e0 b9400121 94000000/fc000000 12000000', 0),
    Site('replace_toast_update', 'code', 'a9be7bfd a9014ff4 910003fd aa0003f3 94000000/fc000000 36000000/fff8001f 3946a268 35000008/ff00001f', 0),
    Site('replace_pane_draw', 'code', '39416008 52801029 6a28013f 54000001/ff00001f a9bd7bfd a90157f6 a9024ff4 910003fd', 0),
    Site('replace_anim_advance', 'code', 'bd405001 1e202028 39415c08 121c6d08 39015c08 54000001/ff00001f d65f03c0 6dbd23e9', 0),
    Site('replace_ui_draw', 'code', '14000000/fc000000 f9013001 b4000001/ff00001f d10403ff a90a7bfd a90b6ffc a90c67fa a90d5ff8', 0),
    Site('replace_tick_call', 'code', '94000000/fc000000 90000008/9f00001f 91000108/ffc003ff f9400100 b4000000/ff00001f b9411ae8 51000508 7100051f', 0),
    Site('replace_fill_call', 'code', '94000000/fc000000 aa1303e0 94000000/fc000000 b9773a75 710006bf b9788274 54000000/ff00001f 35000015/ff00001f', 0),
    Site('replace_chest_return', 'code', 'd65f03c0 f9400674 90000008/9f00001f 91000108/ffc003ff 910023e1 aa1303e0 a900a3f5 94000000/fc000000', 0),
    Site('replace_query_return', 'code', 'b9400121 94000000/fc000000 12000000 a8c17bfd d65f03c0', 4),
    Site('replace_toast_return', 'code', 'd65f03c0 36000000/fff8001f 14000000/fc000000 b9420268 7100011f 5400000d/ff00001f aa1303e0 2a1f03e1', 0),
    Site('replace_toast_tail_a', 'code', '14000000/fc000000 3906a27f 394e2268 7100051f 54000000/ff00001f 7100091f 54000001/ff00001f aa1303e0', 0),
    Site('replace_toast_tail_b', 'code', 'aa1303e0 52800021 94000000/fc000000 aa1303e0 a9414ff4 a8c27bfd 14000000/fc000000', 6),
    Site('replace_ui_draw_native', 'code', '39440008 7100011f 39440809 3943f80a 3943fc0b 7a401920 7a400944 7a421964', 0),
    Site('replace_full_info_call', 'code', '91238108 90000009/9f00001f 91000129/ffc003ff f9000268 90000008/9f00001f 91000108/ffc003ff 91004129 a9022269 f9400bf3 a8c27bfd d65f03c0 90000008/9f00001f 91000108/ffc003ff 91004108 f8010408 14000000/fc000000 a9be7bfd f9000bf3 910003fd 90000008/9f00001f aa0003f3 f9400108/ffc003ff 91004108 f8010408 94000000/fc000000 aa1303e0 f9400bf3 a8c27bfd 14000000/fc000000 52800020 d65f03c0 a9bf7bfd 910003fd 91008000 94000000/fc000000', 34),
    # The read of the record's visit count in the marker update (1.9.0 0x7000d7bea4): the map draws a place's marker
    # from it, so Cheats > Map reads it as 1 for the places it covers (patches.py _place_count_read) - the count in
    # memory is not touched, the game marks and draws the place itself.
    Site('place_count_read', 'code', 'b9401d08 7100011f b9403c09 1211012b 120f7929 b9006008 1a9fd7ea 4a4b3d4c', 0),
    # The travel bit of a place marker (1.9.0 0x7000d7b904): the update computes a marker's flags - 0x400 for its
    # state and 0x200 for "the map may travel here". Cheats > Map replaces the 0x200 select (patches.py
    # _place_travel), so a place the option shows is a travel destination without being activated - no tower
    # activated, so the map fog stays.
    Site('place_travel', 'code', '1a9f116b 2a0b0108 2a0a0108 b9003c08 37000009/fff8001f d65f03c0 a9bd7bfd a90157f6', 0),
    # The tower marker's colour decision (1.9.0 0x7000dac488): the map's marker draw reads the marker's flags and
    # tests the travel bit (0x200) - that test is what draws a tower the option made warpable as an activated one.
    # Cheats > Map hooks the read (patches.py _place_icon) and clears the bit for places that were never visited,
    # so they keep the normal look while staying travel destinations.
    Site('place_icon', 'code', 'b9403e68 7217011f f9406121 a9414ff4 1e200c20 a8c27bfd d61f0020 a9414ff4', 0),
    # The map's completion rate (tools/patches.py _map_clear_rate; runtime/assists.c rt_map_clear_rate): the map
    # screen asks the game whether the save has the GameClear flag and hides its rate block when the answer is no
    # (1.9.0 0x7000dab280: bl isGameClear, then tbz).
    Site('map_clear_rate', 'code', '2a1f03e0 94000000/fc000000 36000000/ff00001f 90007df8/9f00001f f944cf18 '
         'f9400300 94000000/fc000000', 1),
    # The game's memory (tools/patches.py _game_heap_size; runtime/kernel.c kernel_memory_setup): the size nninitStartup
    # asks SetMemoryHeapSize for (mov w0, #0xc3000000, then the 32 MB block malloc keeps), and the root heap's size
    # main hands sead's heap manager (mov w8, #0xc1000000: the heap less those 32 MB)
    Site('game_heap_size', 'code', '52b86000 94000000/fc000000 910023e0 52a04001 94000000/fc000000 f94007e0 52a04001 '
         '94000000/fc000000', 0),
    Site('game_root_heap_size', 'code', '52b82008 52800020 a9027fe8 94000000/fc000000 52800020 94000000/fc000000 '
         '52800020 94000000/fc000000', 0),
    # ... and the resource system's FixedHeap (mov w0, #0x17600000) and the TexArcWork heap made inside it
    # (mov w0, #0xc000000), where texture archives are tried first
    Site('game_fixed_heap_size', 'code', '52a2ec00 94000000/fc000000 f9001a60 b5000000/ff00001f a9434ff4 f94013f5 '
         'a9417bfd 910103ff', 0),
    Site('game_texarc_heap_size', 'code', '52a18000 52800103 52800024 52800025 91004108 a90027e8 94000000/fc000000 '
         'f9009260', 0),
    # ... and the GPU memory pool the models' textures load into: 500 MB (mov w9, #0x1f400000), or 400 MB in the
    # other mode the csel after them picks (mov w11, #0x19000000)
    Site('game_texture_pool', 'code', '52a3e809 f9000feb 52a3200b f9000914 b94003e8 3941427b b940d156 7100091f', 0),
    Site('game_texture_pool_b', 'code', '52a3200b f9000914 b94003e8 3941427b b940d156 7100091f 39414668 9a89017a', 0),
    # ... and the resource manager's ForResourceS / ForResourceL heaps, whose sizes it reads from its settings
    # (ldr w8, [x20, #0xcc] / [x20, #0xc8]): StarterPackMgr, where the resident packs load (Pack/TitleBG.pack), takes
    # what ForResourceL has left
    Site('game_resource_s', 'code', 'b940ce88 a904e3fa a93f27ba 290523f9 94000000/fc000000 b980ae68 9102227c 52a8141b', 0),
    Site('game_resource_l', 'code', 'b940ca88 90000009/9f00001f 91000129/ffc003ff d10043a1 aa1503e0 b90023f6 79004bf7 '
         '39009bf7', 0),
    # ... and StarterPackMgr's heap inside ForResourceL: 225 MB (mov w8, #0xe100000; 20 MB in the mode the csel picks)
    Site('game_starter_pack', 'code', '52a1c208 52a02809 9a881120 94000000/fc000000 aa0003e1 f9012e60 52802000 52800102', 0),
    # ... and the smaller heaps the HD texture pack's own emulator patch (for 1.6.0) also enlarges: UiHeap (28 MB, or
    # 27 MB in the mode the branch before it picks), Havok's HavokMainHeap (45 MB), PhysicsSystem (0x24000: mov w0,
    # #0x4000 / movk w0, #2, lsl #16), PhysicsTempDefault / PhysicsTempLow (384 / 192 KB), GameDataWork (4 MB, in
    # FixedHeap), GameDataHeap (15 MB) and SaveAreaHeap (5 MB)
    Site('game_ui_heap', 'code', '52a03800 90000008/9f00001f 90000009/9f00001f 91000129/ffc003ff f9400108/ffc003ff '
         '910003e1 aa1303e2 52800103', 0),
    Site('game_ui_heap_b', 'code', '52a03600 14000000/fc000000 90000008/9f00001f f9400100/ffc003ff d65f03c0 d65f03c0 '
         'd65f03c0 a9be7bfd', 0),
    Site('game_havok_heap', 'code', '52a05a03 910003e4 aa0103f3 91004115 a90027f5 94000000/fc000000 a9478281 aa1f03e2', 0),
    Site('game_physics_system', 'code', '72a00040 910003e1 aa1303e2 f90ab909 52800103 52800024 52800025 a9002bf5', 0),
    Site('game_physics_temp', 'code', '52a000c0 aa1303e2 52800103 a90023f5 52800024 52800025 94000000/fc000000 f900de80',
         0),
    Site('game_physics_temp_low', 'code', '52a00060 94000000/fc000000 f900e280 528f3301 aa1303e0 f9400268 52800202 '
         'f9401908', 0),
    Site('game_gamedata_work', 'code', '52a00800 52800103 52800024 52800025 91004108 a90027e8 94000000/fc000000 f9002260',
         0),
    Site('game_gamedata_heap', 'code', '52a01e00 aa1503e2 aa1f03e3 52800104 52800025 52800026 52800037 910042d8', 0),
    Site('game_save_area', 'code', '52a00a00 12800005 f9020ff5 911063e1 aa1f03e3 f90213e8 52800104 2a1f03e6', 0),
    Site('main_data', 'data', '', 0),
    Site('main_end', 'end', '', 0),
]

# Sites a version may lack, as a group: when one of them is neither found by its pattern nor listed in the version's
# KNOWN table, the whole group is left out (with its patches) instead of refusing the version. The mouse camera's
# sites go together: the stick input without the linear vertical part would square the mouse's (unclamped) motion.
# Without them the mouse turns the camera through the right stick, as before (runtime/hle/input.c); without the
# second group it turns it without the game's camera inversion options. Without the pause System page's sites it
# has no Achievements button (Achievements are still in Options).
OPTIONAL_GROUPS = [
    ('ui_layout_draw', 'ui_layout_native', 'ui_layout_pane'),
    ('replace_layout_calculate', 'pouch_can_receive', 'replace_toast_start', 'replace_pickup_status',
     'replace_draw_info_reset', 'actor_link_copy', 'replace_interaction_target', 'actor_link_assign',
     'replace_card_prepare', 'replace_card_close', 'replace_component_update', 'replace_ai_child_matches',
     'replace_pickup_call_a', 'replace_pickup_call_b', 'replace_pickup_call_c', 'replace_safe_string_vtable_slot',
     'replace_string_assign', 'replace_card_manager_slot', 'replace_pouch_manager_slot',
     'replace_interaction_manager_slot', 'replace_chest_closed_wait_name', 'replace_weapon_modifier',
     'replace_toast_vtable', 'replace_game_picture_vtable', 'replace_picture_vtable_base', 'replace_popup',
     'replace_item_popup', 'replace_receive', 'replace_card_close_gate', 'replace_card_action',
     'replace_chest_update', 'replace_query', 'replace_toast_update', 'replace_pane_draw', 'replace_anim_advance',
     'replace_ui_draw', 'replace_tick_call', 'replace_fill_call', 'replace_chest_return', 'replace_query_return',
     'replace_toast_return', 'replace_toast_tail_a', 'replace_toast_tail_b', 'replace_ui_draw_native',
     'replace_full_info_call'),
    ('quick_drop_tick', 'quick_drop_actor_call', 'player_drop_forbidden', 'actor_info_lookup',
     'actor_info_has_tag', 'pouch_discard', 'player_equipment', 'pouch_default_name',
     'player_schedule_equip', 'pouch_equipped_snapshot', 'actor_index_assign', 'actor_index_resolve',
     'actor_index_release', 'actor_link_release', 'actor_link_alive', 'pouch_cleanup', 'player_actor',
     'quick_drop_trash_prohibited', 'quick_drop_actor_method', 'actor_info'),
    ('quick_sort_enter', 'quick_sort_menu', 'pouch_page_count', 'pouch_page_item', 'pouch_item_type',
     'quick_sort_modal', 'pouch_sort', 'quick_sort_refill', 'quick_sort_mapping', 'quick_sort_cursor',
     'quick_sort_labels', 'pouch_lock', 'pouch_unlock', 'quick_sort_event', 'quick_sort_player_busy'),
    OPT_TWO_SITES,
    ('lookat_update',),  # (the free camera: without it Home does nothing)
    ('pcvr_matrix_builder', 'pcvr_base_caller', 'pcvr_layer_base_caller', 'pcvr_eye_caller', 'pcvr_proj_builder', 'pcvr_proj_device', 'pcvr_proj_build_caller', 'pcvr_proj_device_caller', 'pcvr_proj_root_return', 'pcvr_proj_sibling_return', 'pcvr_render_begin', 'pcvr_render_begin_caller', 'labo_set_vr', 'labo_end_vr_call', 'labo_enable_rec_call', 'labo_vr_notify_1', 'labo_vr_notify_2', 'labo_sensor_ret', 'labo_track_start_ret', 'pcvr_cam_update_0', 'pcvr_cam_update_1', 'pcvr_cam_update_2', 'pcvr_cam_update_3', 'pcvr_cam_update_4', 'pcvr_cam_update_5', 'pcvr_cam_update_6', 'pcvr_cam_update_7', 'pcvr_actor_vtable', 'pcvr_eye_vtable', 'pcvr_proj_vtable', 'pcvr_base_vtable', 'labo_manager_slot', 'automation_screen_root',),
    ('gyro_scene_actor',),
    ('pcvr_hud_point',),
    ('pcvr_lens_draw', 'pcvr_lens_depth_draw'),
    ('pcvr_screen_draw', 'pcvr_pane_draw', 'pcvr_pane_child_draw'),
    ('pcvr_balloon_point',),
    ('pcvr_marker_store_1', 'pcvr_marker_store_2', 'pcvr_marker_store_3', 'pcvr_marker_store_4', 'pcvr_marker_store_5',
     'pcvr_marker_store_6', 'pcvr_marker_store_7', 'pcvr_marker_store_8', 'pcvr_marker_component_call',
     'pcvr_marker_wheel_follow', 'pcvr_marker_wheel_flat'),  # (1.9.0 only)
    ('pcvr_rune_test', 'pcvr_rune_test_draw'),
    ('pcvr_effect_draw',),  # (1.9.0 only)
    ('pcvr_labo_fov', 'pcvr_lod_cull_matrix', 'pcvr_lod_cull_fov', 'pcvr_vis_cull_fov', 'pcvr_vis_cull_matrix',
     'pcvr_vis2_cull_fov', 'pcvr_vis2_cull_matrix'),  # (1.9.0 only)
    ('pcvr_grass_volume', 'pcvr_grass_projection', 'pcvr_grass_view', 'pcvr_volume_view', 'pcvr_volume_projection',
     'pcvr_volume_copy'),  # (1.9.0 only)
    ('pcvr_model_detail_depth',),  # (1.9.0 only)
    ('pcvr_ui_parallax',),
    ('pcvr_layer_final_draw', 'pcvr_layer_placement_draw', 'pcvr_display_lens_draw', 'pcvr_display_copy_draw'),
    ('surf_contact', 'surf_boost_remove', 'surf_jump_request'),
    ('stale_actor_helper', 'stale_actor_helper2', 'stale_actor_kind', 'stale_actor_target', 'stale_actor_flag',
     'stale_actor_flag2', 'stale_actor_flag3', 'stale_actor_flag4', 'stale_actor_ai_msg'),
    ('quick_menu_capture_alpha',),
    ('cjk_font_size', 'cjk_font_bin', 'cjk_font_quad'),
    ('kf_angle',),
    ('demo_move_arrive',),
    ('rail_point_radius',),
    ('phys_tick', 'phys_tick_mt', 'phys_motion_tick', 'phys_motion_tick_c'),
    ('yaz0_stream', 'yaz0_stream_ret'),
    ('pc_map_focus', 'pc_map_focus_target', 'pc_map_focus_now'),
    ('beast_focus_axes', 'beast_focus_linear', 'beast_confirm_axes', 'beast_confirm_linear',
     'beast_clamp_pitch', 'beast_wrap_yaw', 'beast_world'),
    ('pc_button_group',),
    ('pc_cursor_manager', 'pc_cursor_activate', 'pc_pane_bounds'),
    ('menu_wrap', 'ui_control_layer'),  # (without them the menus stop at their ends, as in the game)
    ('cam_stick', 'cam_pitch_chase', 'cam_pitch_surf', 'cam_pitch_wakeboard', 'cam_pitch_aim', 'cam_pitch_item'),
    ('cam_reverse_x', 'cam_reverse_y'),
    ('pause_button', 'msg_find', 'pause_upgrade_offer', 'pause_upgrade_owned', 'pause_upgrade_text'),
    ('cam_stick_end',),  # (Controls > Controller camera speed: without it the setting does nothing)
    ('traverse_scale', 'traverse_scale_default'),
    ('grass_range', 'grass_fade_blades', 'grass_fade_quads'),
    ('grass_blade_slots', 'grass_blade_cap_a', 'grass_blade_cap_b', 'grass_blade_cap_c', 'grass_blade_cap_c_max',
     'grass_blade_lateral_near',
     'grass_blade_lateral_far'),
    ('tree_ranges',),
    ('act_hold', 'act_trig', 'act_masks'),
    ('pouch_remove_arrows',),
    ('no_wear_store',),
    ('no_wear_hit', 'no_wear_ret'),
    ('link_life_store', 'link_life_tick'),
    ('stamina_store',),
    ('sprint_cost', 'sprint_press_cost', 'battle_info_field', 'battle_info_b', 'battle_info_c'),  # (Cheats > Free sprint: without them it does nothing)
    ('camera_squat_idle', 'camera_enter_start', 'camera_exit_finished', 'camera_exit_active', 'camera_leave'),
    ('horse_crash', 'horse_stop_check'),  # (Cheats > Improved horse controls' stops: without them it does nothing)
    ('horse_ride', 'horse_stick_scale', 'horse_play_rate'),  # (Cheats > Improved horse controls' steering: without them it does nothing)
    ('tp_gate',),  # (a far teleport of the control endpoint: without it a long jump freezes the world)
    ('title_version_text_a', 'title_version_text_b'),  # (the title's version line in full: without them it stops at 15 letters)
    ('item_tick', 'pouch_item_get', 'pouch_mgr'),
    ('actor_create', 'actor_creator', 'actor_create_heap', 'inst_params_vt', 'inst_params_init', 'inst_params_add',
     'inst_params_add_string'),  # (reward chests: without them a reward waits for room in the pouch)
    ('horse_cam_lr',),
    ('game_heap_size', 'game_root_heap_size'),  # (both or neither: the root heap must fit in the heap)
    ('game_fixed_heap_size', 'game_texarc_heap_size'),  # (both or neither: TexArcWork is made inside FixedHeap)
    ('game_texture_pool', 'game_texture_pool_b'),
    ('game_resource_s', 'game_resource_l', 'game_starter_pack'),  # (StarterPackMgr grows inside ForResourceL)
    ('game_ui_heap', 'game_ui_heap_b'),
    ('game_havok_heap', 'game_physics_system', 'game_physics_temp', 'game_physics_temp_low'),
    ('game_gamedata_work',),
    ('game_gamedata_heap', 'game_save_area'),
]
OPTIONAL = {n for g in OPTIONAL_GROUPS for n in g}

KNOWN = {
    '1.9.0': {
        'cjk_font_size': 0x7000017d40,
        'cjk_font_bin': 0x7000017d78,
        'cjk_font_quad': 0x7000017f78,
        'ui_layout_draw': 0x70013ee084,
        'ui_layout_native': 0x7000f6f690,
        'ui_layout_pane': 0x7000027d30,
        'replace_layout_calculate': 0x7000025c64,
        'pouch_can_receive': 0x7000be21e0,
        'replace_toast_start': 0x7000da9a0c,
        'replace_pickup_status': 0x7000e780f0,
        'replace_draw_info_reset': 0x7000f60944,
        'actor_link_copy': 0x7001070ce0,
        'replace_interaction_target': 0x70010d544c,
        'actor_link_assign': 0x700112649c,
        'replace_card_prepare': 0x70013e942c,
        'replace_card_close': 0x70013e99c4,
        'replace_component_update': 0x70013eb788,
        'replace_ai_child_matches': 0x7001537800,
        'replace_pickup_call_a': 0x7000cb0ce4,
        'replace_pickup_call_b': 0x7000ca4440,
        'replace_pickup_call_c': 0x7000cbc270,
        'replace_safe_string_vtable_slot': 0x7001d66260,
        'replace_string_assign': 0x7000573698,
        'replace_card_manager_slot': 0x7001d67960,
        'replace_pouch_manager_slot': 0x7001d67af0,
        'replace_interaction_manager_slot': 0x7001d68838,
        'replace_chest_closed_wait_name': 0x700164f739,
        'replace_weapon_modifier': 0x70007b4ee0,
        'replace_toast_vtable': 0x7001cde098,
        'replace_game_picture_vtable': 0x7001d0e408,
        'replace_picture_vtable_base': 0x7001b7ccd0,
        'replace_popup': 0x7000e78a30,
        'replace_item_popup': 0x7000e7a7f0,
        'replace_receive': 0x7000be1e30,
        'replace_card_close_gate': 0x70013ed264,
        'replace_card_action': 0x7000709ba0,
        'replace_chest_update': 0x7000a9d1bc,
        'replace_query': 0x7000b445c0,
        'replace_toast_update': 0x7000daa3fc,
        'replace_pane_draw': 0x7000027d30,
        'replace_anim_advance': 0x7000f6c9b4,
        'replace_ui_draw': 0x70013ee084,
        'replace_tick_call': 0x7000e66d9c,
        'replace_fill_call': 0x7000e28538,
        'replace_chest_return': 0x7000a9d210,
        'replace_query_return': 0x7000b445e4,
        'replace_toast_return': 0x7000daa4d8,
        'replace_toast_tail_a': 0x7000daa42c,
        'replace_toast_tail_b': 0x7000daa514,
        'replace_ui_draw_native': 0x7000f6f690,
        'replace_full_info_call': 0x70005e6e54,

        'quick_drop_tick': 0x7000e68168,
        'quick_drop_actor_call': 0x7001223f50,
        'player_drop_forbidden': 0x7000cc0334,
        'actor_info_lookup': 0x700108dc0c,
        'actor_info_has_tag': 0x700108e7a0,
        'pouch_discard': 0x7000d9784c,
        'player_equipment': 0x7000570128,
        'pouch_default_name': 0x7000d940c4,
        'player_schedule_equip': 0x7000cbe480,
        'pouch_equipped_snapshot': 0x7000d9f4a8,
        'actor_index_assign': 0x7001517e60,
        'actor_index_resolve': 0x7001517abc,
        'actor_index_release': 0x7001517f8c,
        'actor_link_release': 0x7001126464,
        'actor_link_alive': 0x7001517e0c,
        'pouch_cleanup': 0x7000d97f68,
        'player_actor': 0x7000cc6fd0,
        'quick_drop_trash_prohibited': 0x7000d388b0,
        'quick_drop_actor_method': 0x70007acaa0,
        'actor_info': 0x7001e39450,
        'quick_sort_enter': 0x7000e283fc,
        'quick_sort_menu': 0x7000e28744,
        'pouch_page_count': 0x7000d970a8,
        'pouch_page_item': 0x7000d992ac,
        'pouch_item_type': 0x7000d91414,
        'quick_sort_modal': 0x7000e7e750,
        'pouch_sort': 0x7000d980a0,
        'quick_sort_refill': 0x7000e27c2c,
        'quick_sort_mapping': 0x7000e28070,
        'quick_sort_cursor': 0x7000d665a4,
        'menu_wrap': 0x7000f586b8,
        'ui_control_layer': 0x7000f6e2c8,
        'quick_sort_labels': 0x7000e278c0,
        'pouch_lock': 0x7000eaff08,
        'pouch_unlock': 0x7000eaff18,
        'quick_sort_event': 0x7000d1499c,
        'quick_sort_player_busy': 0x7000d35b80,
        'opt_two_redraw': 0x7000e2eed8,
        'opt_two_select': 0x7000e3047c,
        'opt_two_state': 0x7000e305d0,
        'opt_two_decide': 0x7000e30668,
        'opt_two_after': 0x7000e30a90,
        'opt_two_row_state': 0x7000e302e4,
        'opt_two_update_control': 0x7000e2f7c8,
        'opt_two_initial_control': 0x7000e3019c,
        'opt_two_left': 0x7000e2fa50,
        'opt_two_right': 0x7000e2fae4,
        'opt_two_text': 0x7000e30ee0,
        'opt_two_help': 0x7000e30550,
        'opt_two_pane_calc': 0x70000276a0,
        'lookat_update': 0x7000eb2d5c,
        'yaz0_stream': 0x700131d674,
        'yaz0_stream_ret': 0x700131d7e8,
        'opt_create_rows': 0x7000e2f0a8,
        'opt_row_init_flag_type': 0x7000e2f538,
        'gdt_flag_type': 0x7001136dc8,
        'opt_row_redraw': 0x7000e2eed8,
        'opt_read': 0x7000e2fde4,
        'opt_store': 0x7000e2ee44,
        'opt_table': 0x7001e2e610,
        'opt_reset': 0x7000e31118,
        'opt_msg_call': 0x7000e30f70,
        'msg_lookup': 0x7000e84870,
        'opt_decide': 0x7000e30700,
        'opt_update': 0x7000e2f564,
        'opt_back_call': 0x7000e2f834,
        'screen_back': 0x70013eca68,
        'screen_open_request': 0x7000e833f0,
        'screen_open': 0x70013ed220,
        'screen_close': 0x70013ed264,
        'screen_mgr': 0x7001e374b8,
        'beast_focus_axes': 0x7000df31bc,
        'beast_focus_linear': 0x7000df3438,
        'beast_confirm_axes': 0x7000df326c,
        'beast_confirm_linear': 0x7000df3710,
        'beast_clamp_pitch': 0x7000df38f4,
        'beast_wrap_yaw': 0x7000df3988,
        'beast_world': 0x7001e328d8,
        'pause_options_update': 0x7000dd0384,
        'opt_screen_rows': 0x36b0,
        'dialog_texts': 0x7000e57af8,
        'dialog_back': 0x7000e59714,
        'dialog_answer': 0x7001dac9a8,
        'title_update': 0x7000e5f070,
        'title_button': 0x7000e5f9b0,
        'pause_button': 0x7000dcfbe8,
        'pause_upgrade_offer': 0x7000dcecb8,
        'pause_upgrade_owned': 0x7000dcecc0,
        'pause_upgrade_text': 0x7000dcf02c,
        'msg_find': 0x7000f6a04c,
        'title_options_state': 0x3654,
        'title_msg_file': 0x7001e31f50,
        'title_version_format': 0x70015cd092,
        'title_version_text_a': 0x7000e5eda0,
        'title_version_text_b': 0x7000e5f248,
        'frame_wait': 0x7000e9487c,
        'frame_loop_interval_ret': 0x70011267c4,
        'vfr_ratio': 0x700151d494,
        'gui_speed': 0x7001280ee8,
        'quick_menu_capture_alpha': 0x7000e26a38,
        'day_cycle_time_only': 0x70014266d4,
        'day_cycle_normal': 0x7001426788,
        'arrow_fall_gravity': 0x70005ee734,
        'glide_overspeed_dec_181c': 0x7000c866f0,
        'glide_overspeed_dec_1824': 0x7000c867b4,
        'glide_overspeed_add_181c': 0x7000c86718,
        'glide_overspeed_add_1824': 0x7000c867dc,
        'surf_contact': 0x70011e9e74,
        'surf_boost_remove': 0x70011e9e94,
        'surf_jump_request': 0x70011ea3d4,
        'stale_actor_helper': 0x700120bc40,
        'stale_actor_helper2': 0x700120c740,
        'stale_actor_kind': 0x700120ccdc,
        'stale_actor_target': 0x7000dea400,
        'stale_actor_flag': 0x700120c780,
        'stale_actor_flag2': 0x700120c7f0,
        'stale_actor_flag3': 0x700120c80c,
        'stale_actor_flag4': 0x700120c830,
        'stale_actor_ai_msg': 0x7000bbb670,
        'phys_solver_steps': 0x700156f930,
        'demo_move_arrive': 0x7000c711dc,
        'rail_point_radius': 0x7000c89d4c,
        'kf_angle': 0x70012d57fc,
        'phys_tick': 0x70000f5e20,
        'phys_tick_mt': 0x700012644c,
        'phys_motion_tick': 0x700011f95c,
        'phys_motion_tick_c': 0x70001207cc,
        'snd_frame_floor': 0x7001551004,
        'persp_matrix': 0x7000eb4a50,
        'persp_matrix_offset': 0x7000eb4a58,
        'persp_unproject_y': 0x7000eb4b28,
        'proj_update': 0x7000eb4474,
        'frustum_lr0': 0x7000eb4f24,
        'frustum_lr1': 0x7000eb4f4c,
        'frustum_tb0': 0x7000eb4f60,
        'frustum_tb1': 0x7000eb4f80,
        'persp_unproject_x': 0x7000eb4b58,
        'pause_state': 0x7001e1611c,
        'tp_gate': 0x70010b7a40,
        'event_mgr': 0x7001e3a638,
        'controller_step': 0x7001dad0e0,
        'player_info': 0x7001e13ed0,
        'world_mgr': 0x7001e57580,
        'silent_link': 0x70010d7e1c,
        'hero_soul_wind': 0x7000cd9390,
        'hero_soul_electric': 0x7000cd94a4,
        'hero_soul_fire': 0x7000cd9428,
        'hero_soul_water': 0x7000cd9520,
        'cam_stick': 0x7000d57fe0,
        'cam_stick_end': 0x7000d5801c,
        'horse_cam_lr': 0x7000c29000,
        'cam_pitch_chase': 0x7000bf5334,
        'cam_pitch_surf': 0x7000c1a52c,
        'cam_pitch_wakeboard': 0x7000c22104,
        'cam_pitch_aim': 0x7000bf0bfc,
        'cam_pitch_item': 0x7000bf2c0c,
        'cam_reverse_x': 0x7000d17024,
        'cam_reverse_y': 0x7000d1703c,
        'traverse_scale': 0x70010a4674,
        'traverse_scale_default': 0x70010a4694,
        'grass_range': 0x70013ad8b0,
        'grass_fade_blades': 0x70013a85d0,
        'grass_fade_quads': 0x70013baa5c,
        'grass_blade_slots': 0x70013a7230,
        'grass_blade_cap_a': 0x70013b32e0,
        'grass_blade_cap_b': 0x7001496c40,
        'grass_blade_cap_c': 0x70013ae960,
        'grass_blade_cap_c_max': 0x70013ae96c,
        'grass_blade_lateral_near': 0x70013a8f0c,
        'grass_blade_lateral_far': 0x70013a8f20,
        'tree_ranges': 0x70013c94ac,
        'game_heap_size': 0x700000071c,
        'game_root_heap_size': 0x7000c617c0,
        'game_fixed_heap_size': 0x700131aef8,
        'game_texarc_heap_size': 0x700131b49c,
        'game_texture_pool': 0x700127b6e0,
        'game_texture_pool_b': 0x700127b6e8,
        'game_resource_s': 0x700155fda4,
        'game_resource_l': 0x700155fde0,
        'game_starter_pack': 0x7001209a8c,
        'game_ui_heap': 0x7000e612ec,
        'game_ui_heap_b': 0x7000e61348,
        'game_havok_heap': 0x700156dfc8,
        'game_physics_system': 0x700156e000,
        'game_physics_temp': 0x700156e034,
        'game_physics_temp_low': 0x700156e080,
        'game_gamedata_work': 0x700131b278,
        'game_gamedata_heap': 0x700112b198,
        'game_save_area': 0x700112b360,
        'act_hold': 0x7000d16f0c,
        'act_trig': 0x7000d16f74,
        'act_masks': 0x70016d89b4,
        'pouch_remove_arrows': 0x7000d93814,
        'no_wear_store': 0x7000d93d50,
        'no_wear_hit': 0x7000b9542c,
        'no_wear_ret': 0x70007b09f4,
        'link_life_store': 0x7000cc4e70,
        'link_life_tick': 0x7000cc4da4,
        'stamina_store': 0x7000cc7430,
        'sprint_cost': 0x7000c84854,
        'sprint_press_cost': 0x7000c848a0,
        'battle_info_field': 0x7001345060,
        'battle_info_b': 0x700133dcac,
        'battle_info_c': 0x7001349648,
        'horse_crash': 0x70006b16c4,
        'horse_stop_check': 0x70006b19c8,
        'camera_squat_idle': 0x7000ca9ef0,
        'camera_enter_start': 0x7000caa6f4,
        'camera_exit_finished': 0x7000cabfb4,
        'camera_exit_active': 0x7000cacfac,
        'camera_leave': 0x7000ca3734,
        'horse_ride': 0x70006afe60,
        'horse_stick_scale': 0x70006b1cf4,
        'horse_play_rate': 0x70006b218c,
        'item_tick': 0x7000cc4c3c,
        'pouch_item_get': 0x7000d921bc,
        'pouch_mgr': 0x7001e18570,
        'actor_create': 0x7001535b10,
        'actor_creator': 0x7001e95430,
        'actor_create_heap': 0x7001e14cd8,
        'inst_params_vt': 0x7001bc5530,
        'inst_params_init': 0x700112615c,
        'inst_params_add': 0x7001126168,
        'inst_params_add_string': 0x7000589948,
        'place_count_read': 0x7000d7bea4,
        'place_travel': 0x7000d7b904,
        'place_icon': 0x7000dac488,
        'map_clear_rate': 0x7000dab280,
        'main_data': 0x7001b7a000,
        'pcvr_matrix_builder': 0x7000eb2d5c,
        'pcvr_base_caller': 0x70010ec518,
        'pcvr_layer_base_caller': 0x7000eef164,
        'pcvr_eye_caller': 0x7000eef800,
        'pcvr_proj_builder': 0x7000eb4a44,
        'pcvr_proj_device': 0x7000eb4684,
        'pcvr_proj_build_caller': 0x7000eb4494,
        'pcvr_proj_device_caller': 0x7000eb44b8,
        'pcvr_proj_root_return': 0x7000ef1d40,
        'pcvr_proj_sibling_return': 0x7000eef428,
        'pcvr_render_begin': 0x7000f747b4,
        'pcvr_render_begin_caller': 0x7000ef0310,
        'labo_set_vr': 0x7001260e20,
        'labo_end_vr_call': 0x7001260ee8,
        'labo_enable_rec_call': 0x7001260eec,
        'labo_vr_notify_1': 0x7000e8a360,
        'labo_vr_notify_2': 0x7000e8a410,
        'labo_sensor_ret': 0x7001260f68,
        'labo_track_start_ret': 0x7001266254,
        'pcvr_cam_update_0': 0x7000bf4e74,
        'pcvr_cam_update_1': 0x7000c1ce84,
        'pcvr_cam_update_2': 0x7000bf8858,
        'pcvr_cam_update_3': 0x7000c1a30c,
        'pcvr_cam_update_4': 0x7000c21ee8,
        'pcvr_cam_update_5': 0x7000c245b8,
        'pcvr_cam_update_6': 0x7000c0e290,
        'pcvr_cam_update_7': 0x7000c14218,
        'pcvr_actor_vtable': 0x7001cc64b0,
        'pcvr_eye_vtable': 0x7001d05158,
        'pcvr_proj_vtable': 0x7001d05380,
        'pcvr_base_vtable': 0x7001d1eee0,
        'labo_manager_slot': 0x7001d66c98,
        'automation_screen_root': 0x7001d66d58,
        'pcvr_hud_point': 0x7000e1dec0,
        'pcvr_lens_draw': 0x700125a8d0,
        'pcvr_lens_depth_draw': 0x7001261298,
        'pcvr_screen_draw': 0x7000f714c0,
        'pcvr_pane_draw': 0x7000027d80,
        'pcvr_pane_child_draw': 0x7000027dc8,
        'pcvr_balloon_point': 0x70013f0a50,
        'pcvr_marker_store_1': 0x7000e1ba70,
        'pcvr_marker_store_2': 0x7000e1c1c0,
        'pcvr_marker_store_3': 0x7000e1cdb4,
        'pcvr_marker_store_4': 0x7000e1ce0c,
        'pcvr_marker_store_5': 0x7000e1ce48,
        'pcvr_marker_store_6': 0x7000e1ceec,
        'pcvr_marker_store_7': 0x7000e1d430,
        'pcvr_marker_store_8': 0x7000e1d6a4,
        'pcvr_marker_component_call': 0x7000e1ad88,
        'pcvr_marker_wheel_follow': 0x7000e1b288,
        'pcvr_marker_wheel_flat': 0x7000e1aeb4,
        'pcvr_rune_test': 0x700126f798,
        'pcvr_rune_test_draw': 0x700126acbc,
        'pcvr_effect_draw': 0x700105d054,
        'pcvr_labo_fov': 0x700126138c,
        'pcvr_lod_cull_matrix': 0x70010bb2a0,
        'pcvr_lod_cull_fov': 0x70010bb2d4,
        'pcvr_vis_cull_fov': 0x70010ec5c0,
        'pcvr_vis_cull_matrix': 0x70010ec5d8,
        'pcvr_vis2_cull_fov': 0x70010ec6a0,
        'pcvr_vis2_cull_matrix': 0x70010ec6b8,
        'pcvr_grass_volume': 0x70014906f4,
        'pcvr_grass_projection': 0x70014907a0,
        'pcvr_grass_view': 0x70014907c8,
        'pcvr_volume_view': 0x7000f8f780,
        'pcvr_volume_projection': 0x7000f90cf4,
        'pcvr_volume_copy': 0x700101a468,
        'pcvr_model_detail_depth': 0x70010575e0,
        'pcvr_ui_parallax': 0x7000eeee50,
        'pcvr_layer_final_draw': 0x700126c080,
        'pcvr_layer_placement_draw': 0x7000fec358,
        'pcvr_display_lens_draw': 0x7000ec4ed0,
        'pcvr_display_copy_draw': 0x7000ec4f2c,
        'main_end': 0x7001e98000,
    },
}


class SiteError(Exception):
    pass


def supported_version(build_id):
    """The VERSIONS entry of a main module build id (64 hex digits or the first 16), or None."""
    for v in VERSIONS:
        if v.build_id and build_id[:16] == v.build_id:
            return v
    return None


# ------------------------------------------------------------------ instructions

def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def is_adrp(w):
    return (w & 0x9F000000) == 0x90000000


def adrp_page(w, pc):
    return (pc & ~0xFFF) + (sext(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21) << 12)


def lo12(w):
    """(scaled low 12 bits, base register) of an ADD (immediate, 64-bit) or an LDR / STR (unsigned offset); else None"""
    if (w & 0xFFC00000) == 0x91000000:  # add xd, xn, #imm
        return (w >> 10) & 0xFFF, (w >> 5) & 31
    if (w & 0x3B000000) == 0x39000000:  # ldr / str (immediate, unsigned offset), integer or SIMD&FP
        scale = (w >> 30) & 3
        if w & 0x04000000 and (w >> 23) & 1:  # (a 128-bit SIMD&FP register)
            scale = 4
        return ((w >> 10) & 0xFFF) << scale, (w >> 5) & 31
    return None


def imm_value(w):
    """The immediate of an LDR / STR (unsigned offset, scaled), an ADD / SUB / CMP (immediate) or a MOVZ; else None"""
    if (w & 0x3B000000) == 0x39000000:
        return lo12(w)[0]
    if (w & 0x1F000000) == 0x11000000:
        return ((w >> 10) & 0xFFF) << (12 if (w >> 22) & 1 else 0)
    if (w & 0x7F800000) == 0x52800000:  # movz (32 / 64-bit)
        return ((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 3))
    return None


def mask(w, adrp_regs):
    """The bits of instruction w a pattern keeps (adrp_regs: registers an ADRP earlier in the pattern set)."""
    if (w & 0x7C000000) == 0x14000000:  # b, bl
        return 0xFC000000
    if (w & 0xFF000010) == 0x54000000 or (w & 0x7E000000) == 0x34000000:  # b.cond; cbz, cbnz
        return 0xFF00001F
    if (w & 0x7E000000) == 0x36000000:  # tbz, tbnz
        return 0xFFF8001F
    if (w & 0x1F000000) == 0x10000000:  # adr, adrp
        return 0x9F00001F
    if (w & 0x3B000000) == 0x18000000:  # ldr (literal)
        return 0xFF00001F
    lo = lo12(w)
    if lo and lo[1] in adrp_regs:
        return 0xFFC003FF
    return 0xFFFFFFFF


def make_pattern(words):
    """A pattern string for these instruction words: each word as hex, or value/mask where bits are masked."""
    out, adrp_regs = [], set()
    for w in words:
        m = mask(w, adrp_regs)
        if is_adrp(w):
            adrp_regs.add(w & 31)
        else:  # (most instructions write their bits 0-4 register: it holds no page any more)
            adrp_regs.discard(w & 31)
        out.append(f'{w:08x}' if m == 0xFFFFFFFF else f'{w & m:08x}/{m:08x}')
    return ' '.join(out)


def parse_pattern(p):
    words = []
    for t in p.split():
        v, _, m = t.partition('/')
        words.append((int(v, 16), int(m, 16) if m else 0xFFFFFFFF))
    return words


def compile_pattern(p):
    parts = []
    for v, m in parse_pattern(p):
        for i in range(4):
            vb, mb = (v >> 8 * i) & 0xFF, (m >> 8 * i) & 0xFF
            if mb == 0xFF:
                parts.append(re.escape(bytes([vb])))
            elif mb == 0:
                parts.append(b'.')
            else:
                parts.append(b'[' + b''.join(re.escape(bytes([b])) for b in range(256) if b & mb == vb & mb) + b']')
    return re.compile(b''.join(parts), re.DOTALL)


# ------------------------------------------------------------------ a main module

class Main:
    def __init__(self, nso, base=MAIN_BASE):
        self.nso, self.base = nso, base
        self.text_off, self.text_size = nso.text
        self.text = bytes(nso.image[self.text_off:self.text_off + self.text_size])

    def word(self, a):
        return struct.unpack_from('<I', self.nso.image, a - self.base)[0]

    def find(self, pattern, limit=2):
        """Addresses (word-aligned, in .text) where the pattern matches; at most `limit`."""
        rx, out, pos = compile_pattern(pattern), [], 0
        while len(out) < limit:
            m = rx.search(self.text, pos)
            if not m:
                break
            if m.start() % 4:
                pos = m.start() + 1
                continue
            out.append(self.base + self.text_off + m.start())
            pos = m.start() + 4
        return out

    def target(self, kind, a):
        """What the site of this kind at instruction a is (see the module docstring)."""
        w = self.word(a)
        if kind == 'code':
            return a
        if kind == 'call':
            if (w & 0x7C000000) != 0x14000000:
                raise SiteError(f'{a:#x} is not a branch ({w:08x})')
            return a + sext(w & 0x3FFFFFF, 26) * 4
        if kind == 'imm':
            v = imm_value(w)
            if v is None:
                raise SiteError(f'{a:#x} has no immediate ({w:08x})')
            return v
        if is_adrp(w):
            v = adrp_page(w, a)
        else:
            lo = lo12(w)
            if not lo:
                raise SiteError(f'{a:#x} does not complete an address ({w:08x})')
            for b in range(a - 4, a - 4 * 33, -4):  # its ADRP
                pw = self.word(b)
                if is_adrp(pw) and (pw & 31) == lo[1]:
                    v = adrp_page(pw, b) + lo[0]
                    break
            else:
                raise SiteError(f'{a:#x}: no ADRP for x{lo[1]} above it')
        if kind == 'got':
            return self.base + struct.unpack_from('<Q', self.nso.image, v - self.base)[0]
        return v

    def locate(self, s):
        """(address, or None when the pattern doesn't find exactly one place; how many places it matches) of a site"""
        if s.kind == 'data':
            return self.base + self.nso.data[0], 1
        if s.kind == 'end':
            return self.base + self.nso.size, 1
        hits = []
        for p, at in s.pattern if isinstance(s.pattern, list) else [(s.pattern, s.at)] if s.pattern else []:
            hits = self.find(p)
            if len(hits) == 1:
                return self.target(s.kind, hits[0] + 4 * at), 1
        return None, len(hits)

    def text_words(self, a, n):
        return [self.word(a + 4 * i) for i in range(n)]

    def function_of(self, a):
        """(start, end) of the function holding a, from the module's unwind info (None: none there)."""
        import bisect
        from analyze import eh_frame_extents
        if not hasattr(self, 'fdes'):
            self.fdes = sorted((self.base + s, self.base + e) for s, e in eh_frame_extents(self.nso) if e > s)
        i = bisect.bisect_right(self.fdes, (a, 1 << 64)) - 1
        return self.fdes[i] if i >= 0 and a < self.fdes[i][1] else None

    def pattern_for(self, a, kind='code', shortest=8, longest=24):
        """A pattern that finds only a: (pattern, at). It stays inside a's function (the code around it may change
        on its own), starts as close above a as it can - at the ADRP for a ref or got site, whose address bits it
        masks then - and has `shortest` words at least (or the whole function): short ones are likelier to find
        another place in a new build."""
        f0, f1 = self.function_of(a) or (a - 64, a + 128)
        first = 0
        if kind in ('ref', 'got') and not is_adrp(self.word(a)):
            reg = lo12(self.word(a))[1]
            first = next(i for i in range(1, 33) if is_adrp(self.word(a - 4 * i)) and self.word(a - 4 * i) & 31 == reg)
        for back in range(first, (a - f0) // 4 + 1):
            s = a - 4 * back
            room = (f1 - s) // 4
            for n in range(min(max(shortest, back + 1), room), min(longest, room) + 1):
                p = make_pattern(self.text_words(s, n))
                if self.find(p) == [s]:
                    return p, back
        raise SiteError(f'no pattern inside its function finds only {a:#x}')


def common_pattern(ma, a, mb, b, kind='code', shortest=8, longest=32):
    """A pattern that finds only a in main module ma and only b in mb, the same code in two versions: the bits both
    keep (and make_pattern keeps). For an imm site the immediate itself is always masked. (pattern, at)"""
    fa, fb = ma.function_of(a) or (a - 64, a + 128), mb.function_of(b) or (b - 64, b + 128)
    first = 0
    if kind in ('ref', 'got') and not is_adrp(ma.word(a)):
        reg = lo12(ma.word(a))[1]
        first = next(i for i in range(1, 33) if is_adrp(ma.word(a - 4 * i)) and ma.word(a - 4 * i) & 31 == reg)
    for back in range(first, min(a - fa[0], b - fb[0]) // 4 + 1):
        sa, sb = a - 4 * back, b - 4 * back
        room = min(fa[1] - sa, fb[1] - sb) // 4
        for n in range(min(max(shortest, back + 1), room), min(longest, room) + 1):
            out = []
            for i, ((va, ka), (vb, kb)) in enumerate(zip(parse_pattern(make_pattern(ma.text_words(sa, n))),
                                                         parse_pattern(make_pattern(mb.text_words(sb, n))))):
                k = ka & kb & ~(va ^ vb) & 0xFFFFFFFF
                if kind == 'imm' and i == back:
                    k &= 0xFFE0001F if (va & 0x7F800000) == 0x52800000 else 0xFFC003FF
                out.append(f'{va & k:08x}' if k == 0xFFFFFFFF else f'{va & k:08x}/{k:08x}')
            p = ' '.join(out)
            if ma.find(p) == [sa] and mb.find(p) == [sb]:
                return p, back
    raise SiteError(f'no pattern inside both functions finds only {a:#x} and {b:#x}')


def resolve(main, known=None, log=None):
    """{site name: address} for this main module. known: the addresses listed for its build (or None)."""
    out, problems = {}, []
    for s in SITES:
        try:
            found, hits = main.locate(s)
        except SiteError as e:
            problems.append(f'{s.name}: {e}')
            continue
        # This experiment changes native object routing. A stale KNOWN address
        # must never turn it on after a failed signature check.
        if s.name in OPT_TWO_SITES and (found is None or
                                      (known and s.name in known and found != known[s.name])):
            if log:
                log(f'game_sites: {s.name}: spike signature did not match')
            continue
        k = (known or {}).get(s.name)
        if found is not None and k is not None and found != k:
            raise SiteError(f'{s.name}: the pattern finds {found:#x}, the table says {k:#x}')
        if found is None and k is None:
            if s.name in OPTIONAL:
                continue
            problems.append(f'{s.name}: the pattern matches {hits} places' + (' or more' if hits > 1 else ''))
            continue
        if found is None and log:
            log(f'game_sites: {s.name}: the pattern matches {hits} places; {k:#x} from the table')
        out[s.name] = found if found is not None else k
    if problems:
        raise SiteError('; '.join(problems))
    for g in OPTIONAL_GROUPS:
        missing = [n for n in g if n not in out]
        if not missing:
            continue
        if log:
            log(f'game_sites: {", ".join(missing)} not found: {", ".join(g)} left out')
        for n in g:
            out.pop(n, None)
    return out


def for_module(nso, base, allow_unsupported=False, log=None):
    """(version name, sites) for the main module nso at base. Refuses a build that VERSIONS doesn't support, unless
    allow_unsupported (bringing up a new version: its sites must then all be found by pattern)."""
    v = supported_version(nso.build_id)
    if not v or not v.supported:
        what = f'version {v.name}, not supported yet' if v else 'not a version Savage Wind knows'
        if not allow_unsupported:
            raise SiteError(f'main module build {nso.build_id[:16]}: {what}')
        if log:
            log(f'game_sites: main module build {nso.build_id[:16]}: {what}; sites by pattern only')
    name = v.name if v else nso.build_id[:16]
    return name, resolve(Main(nso, base), KNOWN.get(name) if v and v.supported else None, log)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('find', 'pattern', 'common'):
        sys.exit(__doc__)
    from nso import Nso
    m = Main(Nso(sys.argv[2]))
    if sys.argv[1] == 'common':
        mb, a, b = Main(Nso(sys.argv[4])), int(sys.argv[3], 16), int(sys.argv[5], 16)
        kind = sys.argv[6] if len(sys.argv) > 6 else 'code'
        p, at = common_pattern(m, a, mb, b, kind)
        print(f"    Site('<name>', '{kind}', '{p}', {at}),  # -> {m.target(kind, a):#x} / {mb.target(kind, b):#x}")
        return
    if sys.argv[1] == 'pattern':
        a = int(sys.argv[3], 16)
        kind = sys.argv[4] if len(sys.argv) > 4 else 'code'
        p, at = m.pattern_for(a, kind)
        print(f"    Site('<name>', '{kind}', '{p}', {at}),  # -> {m.target(kind, a):#x}")
        return
    v = supported_version(m.nso.build_id)
    known = KNOWN.get(v.name, {}) if v else {}
    print(f'main module build {m.nso.build_id[:16]}: ' + (f'version {v.name}' + ('' if v.supported else
          ' (not supported yet)') if v else 'not in VERSIONS'))
    bad = 0
    for s in SITES:
        try:
            found, hits = m.locate(s)
        except SiteError as e:
            found, hits = None, str(e)
        k = known.get(s.name)
        state = (f'{hits} matches' if found is None else
                 'ok' if k == found else 'new' if k is None else f'TABLE SAYS {k:#x}')
        bad += state not in ('ok', 'new')
        print(f"        '{s.name}': {found:#x},  # {state}" if found is not None else
              f"        # '{s.name}': {state}" + (f' (table {k:#x})' if k is not None else ''))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
