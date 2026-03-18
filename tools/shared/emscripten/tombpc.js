// TOMBPC.DAT binary script parser for TRX WebGL builds.
//
// Parses the classic TR2/TR3 game script format and translates binary
// opcode sequences into TRX JSON5 gameflow events.  Also provides a
// minimal JSON5 parser used across the gameflow pipeline.
//
// Reference: tools/tr2/read_tombpc_script (Python, authoritative)
//
// No dependencies (pure data transformation).

'use strict';

// ---------------------------------------------------------------------------
// JSON5 parser
// ---------------------------------------------------------------------------

// Minimal JSON5 → JSON parser (handles // comments + trailing commas)
function _trxParseJSON5(text) {
    text = text.replace(/\/\/.*$/gm, '');
    text = text.replace(/,\s*([\]}])/g, '$1');
    return JSON.parse(text);
}

// ---------------------------------------------------------------------------
// TOMBPC.DAT binary script parser (TR2/TR3)
// ---------------------------------------------------------------------------

// Binary gameflow opcodes — matches tools/tr2/read_tombpc_script GameFlowEvent.
var _GFE = {
    PICTURE:          0x0000,
    LIST_START:       0x0001,
    LIST_END:         0x0002,
    PLAY_FMV:         0x0003,
    START_LEVEL:      0x0004,
    CUTSCENE:         0x0005,
    LEVEL_COMPLETE:   0x0006,
    DEMO_PLAY:        0x0007,
    JUMP_TO_SEQ:      0x0008,
    END_SEQ:          0x0009,
    SET_TRACK:        0x000A,
    SUNSET:           0x000B,
    LOADING_PIC:      0x000C,
    DEADLY_WATER:     0x000D,
    REMOVE_WEAPONS:   0x000E,
    GAME_COMPLETE:    0x000F,
    CUT_ANGLE:        0x0010,
    NO_FLOOR:         0x0011,
    ADD_TO_INV:       0x0012,
    START_ANIM:       0x0013,
    NUM_SECRETS:      0x0014,
    KILL_TO_COMPLETE: 0x0015,
    REMOVE_AMMO:      0x0016,
};

// Opcodes that consume a 16-bit argument after the opcode word.
var _GFE_HAS_ARG = {};
_GFE_HAS_ARG[_GFE.PICTURE]      = 1;
_GFE_HAS_ARG[_GFE.PLAY_FMV]     = 1;
_GFE_HAS_ARG[_GFE.START_LEVEL]  = 1;
_GFE_HAS_ARG[_GFE.CUTSCENE]     = 1;
_GFE_HAS_ARG[_GFE.DEMO_PLAY]    = 1;
_GFE_HAS_ARG[_GFE.JUMP_TO_SEQ]  = 1;
_GFE_HAS_ARG[_GFE.SET_TRACK]    = 1;
_GFE_HAS_ARG[_GFE.LOADING_PIC]  = 1;
_GFE_HAS_ARG[_GFE.CUT_ANGLE]    = 1;
_GFE_HAS_ARG[_GFE.NO_FLOOR]     = 1;
_GFE_HAS_ARG[_GFE.ADD_TO_INV]   = 1;
_GFE_HAS_ARG[_GFE.START_ANIM]   = 1;
_GFE_HAS_ARG[_GFE.NUM_SECRETS]  = 1;

// ADD_TO_INV item enum → TRX object_id key mapping.
// Values verified against src/trx/game/objects/names.def.
// arg < 1000 → add_secret_reward, arg ≥ 1000 → give_item (arg - 1000).
var _TR2_INV_KEYS = [
    'pistols',                // 0
    'autos',                  // 1
    'uzis',                   // 2
    'shotgun',                // 3
    'harpoon_gun',            // 4
    'm16',                    // 5
    'grenade_launcher',       // 6
    'pistols_ammo',           // 7
    'autos_ammo',             // 8
    'uzis_ammo',              // 9
    'shotgun_ammo',           // 10
    'harpoon_gun_ammo',       // 11
    'm16_ammo',               // 12
    'grenade_launcher_ammo',  // 13
    'flare',                  // 14
    'small_medipack',         // 15
    'large_medipack',         // 16
    'pickup_1',               // 17
    'pickup_2',               // 18
    'puzzle_1',               // 19
    'puzzle_2',               // 20
    'puzzle_3',               // 21
    'puzzle_4',               // 22
    'key_1',                  // 23
    'key_2',                  // 24
    'key_3',                  // 25
    'key_4',                  // 26
];

// Parse a TOMBPC.DAT game script (TR2/TR3 format) and return structured
// data including per-level translated sequences.  Returns null on failure.
function _trxParseTR2Script(rawData) {
    try {
        var data = new Uint8Array(rawData);
        var view = new DataView(data.buffer);

        // --- Header ---
        var version = view.getUint32(0, true);
        if (version !== 3) return null;
        var optSize = view.getUint16(260, true);
        var optStart = 262;

        // --- Options block ---
        var numLevels = view.getUint16(optStart + 64, true);
        var numCutscenes = view.getUint16(optStart + 72, true);
        var titleSoundId = view.getUint16(optStart + 76, true);
        var cypher = data[optStart + 120];
        if (numLevels < 2 || numLevels > 100) return null;

        // --- String block reader ---
        function readBlock(pos, count) {
            var offsets = [];
            for (var i = 0; i < count; i++)
                offsets.push(view.getUint16(pos + i * 2, true));
            pos += count * 2;
            var dlen = view.getUint16(pos, true); pos += 2;
            var strings = [];
            for (var i = 0; i < count; i++) {
                var j = pos + offsets[i];
                var s = '';
                while (j < pos + dlen) {
                    var ch = data[j] ^ cypher;
                    j++;
                    if (ch === 0) break;
                    s += String.fromCharCode(ch);
                }
                strings.push(s);
            }
            return { strings: strings, end: pos + dlen };
        }

        // --- Level titles (first block after options) ---
        var pos = optStart + optSize;
        var titleBlock = readBlock(pos, numLevels);
        var levelTitles = titleBlock.strings;

        // --- Scan for level filenames block ---
        // First block of numLevels ascending offsets where every
        // decrypted string is clean ASCII containing a level extension.
        var filenames = null;
        var fnEndPos = 0;
        for (var sp = titleBlock.end;
             sp < data.length - numLevels * 2 - 4; sp++) {
            var first = view.getUint16(sp, true);
            if (first !== 0) continue;
            var ok = true;
            for (var k = 1; k < numLevels; k++) {
                if (view.getUint16(sp + k * 2, true)
                    <= view.getUint16(sp + (k - 1) * 2, true)) {
                    ok = false; break;
                }
            }
            if (!ok) continue;
            try {
                var blk = readBlock(sp, numLevels);
                var allValid = true;
                for (var k = 0; k < blk.strings.length; k++) {
                    var s = blk.strings[k];
                    var sl = s.toLowerCase();
                    if (sl.indexOf('.tr2') < 0
                        && sl.indexOf('.phd') < 0
                        && sl.indexOf('.tub') < 0
                        && sl.indexOf('.psx') < 0) {
                        allValid = false; break;
                    }
                    for (var c = 0; c < s.length; c++) {
                        var cc = s.charCodeAt(c);
                        if (cc < 32 || cc > 126) {
                            allValid = false; break;
                        }
                    }
                    if (!allValid) break;
                }
                if (allValid) {
                    filenames = blk.strings;
                    fnEndPos = blk.end;
                    break;
                }
            } catch (e) {}
        }
        if (!filenames) return null;

        // --- Skip cutscene filenames ---
        var scriptPos = fnEndPos;
        if (numCutscenes > 0) {
            try {
                var cutBlk = readBlock(scriptPos, numCutscenes);
                scriptPos = cutBlk.end;
            } catch (e) {}
        }

        // --- Script sequences ---
        var scriptOffsets = [];
        for (var i = 0; i <= numLevels; i++) {
            scriptOffsets.push(
                view.getUint16(scriptPos + i * 2, true));
        }
        scriptPos += (numLevels + 1) * 2;
        var scriptSize = view.getUint16(scriptPos, true);
        scriptPos += 2;
        var scriptBase = scriptPos;

        // Read raw opcodes for one sequence into an array of {op, arg}.
        function readOpcodes(seqIdx) {
            var sStart = scriptBase + scriptOffsets[seqIdx];
            var sEnd = (seqIdx + 1 < scriptOffsets.length)
                ? scriptBase + scriptOffsets[seqIdx + 1]
                : scriptBase + scriptSize;
            var ops = [];
            for (var p = sStart; p + 1 < sEnd; ) {
                var op = view.getUint16(p, true); p += 2;
                if (op === _GFE.END_SEQ) break;
                var arg = -1;
                if (_GFE_HAS_ARG[op]) {
                    arg = (p + 1 < sEnd) ? view.getUint16(p, true) : -1;
                    p += 2;
                }
                ops.push({ op: op, arg: arg });
            }
            return ops;
        }

        // Translate a raw opcode array into a TRX sequence + music_track.
        // Mirrors tools/tr2/read_tombpc_script:transform_script().
        function translateSequence(ops) {
            var pre = [];      // events before loop_game
            var post = [];     // events after loop_game
            var musicTrack = -1;
            var seenLevel = false;
            var isGameComplete = false;

            for (var i = 0; i < ops.length; i++) {
                var op = ops[i].op;
                var arg = ops[i].arg;
                var target = seenLevel ? post : pre;

                switch (op) {
                case _GFE.SET_TRACK:
                    musicTrack = arg;
                    break;

                case _GFE.START_LEVEL:
                    seenLevel = true;
                    break;

                case _GFE.PLAY_FMV:
                    target.push({ type: 'play_fmv', fmv_id: arg });
                    break;

                case _GFE.CUTSCENE:
                    target.push({ type: 'play_cutscene', cutscene_id: arg });
                    break;

                case _GFE.LEVEL_COMPLETE:
                    // Per Python reference: play_music + level_stats + level_complete
                    post.push({ type: 'play_music', music_track: 41 });
                    post.push({ type: 'level_stats' });
                    post.push({ type: 'level_complete' });
                    break;

                case _GFE.GAME_COMPLETE:
                    isGameComplete = true;
                    post.push({ type: 'total_stats' });
                    break;

                case _GFE.SUNSET:
                    target.push({ type: 'enable_sunset' });
                    break;

                case _GFE.REMOVE_WEAPONS:
                    target.push({ type: 'remove_weapons' });
                    break;

                case _GFE.REMOVE_AMMO:
                    target.push({ type: 'remove_ammo' });
                    break;

                case _GFE.NO_FLOOR:
                    target.push({ type: 'disable_floor', height: arg });
                    break;

                case _GFE.START_ANIM:
                    target.push({ type: 'set_lara_start_anim', anim: arg });
                    break;

                case _GFE.ADD_TO_INV:
                    if (arg >= 0) {
                        var itemIdx = arg >= 1000 ? arg - 1000 : arg;
                        var invType = arg >= 1000 ? 'give_item' : 'add_secret_reward';
                        var objKey = _TR2_INV_KEYS[itemIdx];
                        if (objKey) {
                            target.push({ type: invType, object_id: objKey });
                        }
                    }
                    break;

                // Opcodes we skip (no TRX equivalent or not useful for
                // custom levels):
                // PICTURE, LIST_START, LIST_END, DEMO_PLAY, JUMP_TO_SEQ,
                // LOADING_PIC, DEADLY_WATER, CUT_ANGLE, NUM_SECRETS,
                // KILL_TO_COMPLETE
                }
            }

            // Assemble: pre-events + loop_game + post-events
            var sequence = pre.slice();
            sequence.push({ type: 'loop_game' });
            for (var i = 0; i < post.length; i++) {
                sequence.push(post[i]);
            }

            // If no LEVEL_COMPLETE or GAME_COMPLETE was emitted, add
            // a default level_stats + level_complete.
            if (!isGameComplete) {
                var hasComplete = false;
                for (var i = 0; i < post.length; i++) {
                    if (post[i].type === 'level_complete') {
                        hasComplete = true;
                        break;
                    }
                }
                if (!hasComplete) {
                    sequence.push({ type: 'level_stats' });
                    sequence.push({ type: 'level_complete' });
                }
            }

            return { sequence: sequence, music_track: musicTrack };
        }

        // Extract music tracks (for backward compat) and full sequences.
        var musicTracks = [];
        var sequences = [];
        for (var i = 0; i < numLevels; i++) {
            musicTracks.push(-1);
            sequences.push(null);
        }

        for (var seq = 0; seq < numLevels; seq++) {
            var ops = readOpcodes(seq);
            var result = translateSequence(ops);

            // Find the START_LEVEL target to map music/sequence to the
            // correct file index.
            var levelTarget = -1;
            for (var i = 0; i < ops.length; i++) {
                if (ops[i].op === _GFE.START_LEVEL) {
                    levelTarget = ops[i].arg;
                    break;
                }
            }

            var idx = (levelTarget >= 0 && levelTarget < numLevels)
                ? levelTarget : seq;
            musicTracks[idx] = result.music_track;
            sequences[idx] = result.sequence;
        }

        // CD audio offset derivation (same as before).
        var cdOffset = titleSoundId > 0
            ? titleSoundId - 60 : 4;

        return {
            filenames: filenames,
            levelTitles: levelTitles,
            musicTracks: musicTracks,
            sequences: sequences,
            titleSoundId: titleSoundId,
            cdOffset: cdOffset,
        };
    } catch (e) {
        return null;
    }
}
