// Gameflow setup / translation for TRX WebGL builds.
//
// Orchestrates custom level gameflow generation: detects the source
// format (TRX-native JSON5, TR2X template, TOMBPC.DAT binary, or bare
// level files), patches or generates a gameflow.json5, and writes it
// to the Emscripten VFS.
//
// Depends on: Emscripten FS      (global `FS`)
//             _trxParseJSON5     (from tombpc.js)
//             _trxParseTR2Script (from tombpc.js)

'use strict';

// ---------------------------------------------------------------------------
// Custom gameflow setup
// ---------------------------------------------------------------------------

// Set up the gameflow + strings for a custom level profile.
// If the upload already contains a gameflow.json5 (TRX-native custom
// level), it is used as-is.  Otherwise one is generated from the
// template or TOMBPC.DAT.  `mappedFiles` is mutated: new entries may
// be appended.
function _trxSetupCustomGameflow(mappedFiles, modDir, templateMod, useOutfitImport) {
    var modPrefix = 'games/' + modDir + '/';
    var defaultOutfit = useOutfitImport
        ? 'level_default'
        : templateMod.replace('-level', '_classic');

    // Inject 'level_default' outfit so custom levels can display the
    // Lara meshes baked into the level file (backwards compatibility).
    if (useOutfitImport) {
        _trxInjectLevelDefaultOutfit(modDir, templateMod);
    }

    // Check if the upload already included a gameflow.json5 or
    // a classic game script (TOMBPC.DAT).
    var hasGameflow = false;
    var hasStrings = false;
    var scriptEntry = null;
    for (var i = 0; i < mappedFiles.length; i++) {
        if (mappedFiles[i].path === modPrefix + 'gameflow.json5') {
            hasGameflow = true;
        }
        if (mappedFiles[i].path.indexOf(modPrefix + 'strings') === 0
            && mappedFiles[i].path.endsWith('.json5')) {
            hasStrings = true;
        }
        if (mappedFiles[i].path === modPrefix + 'tombpc.dat') {
            scriptEntry = mappedFiles[i];
        }
    }

    if (hasGameflow) {
        // TRX-native level — patch missing required fields and
        // generate strings that match the provided level count.
        var gfEntry = null;
        for (var i = 0; i < mappedFiles.length; i++) {
            if (mappedFiles[i].path === modPrefix + 'gameflow.json5') {
                gfEntry = mappedFiles[i];
                break;
            }
        }
        var gfText = new TextDecoder().decode(gfEntry.data);

        // Detect TR2X template gameflows (have PLACEHOLDER paths,
        // data/injections/ prefixes, etc.) — these are incompatible
        // with TRX and should be skipped in favour of auto-generation.
        if (gfText.indexOf('"PLACEHOLDER"') !== -1) {
            // Look for the full gameflow (cfg/tr2/ or cfg/tr1/)
            // which has correct music_track, injections, etc.
            var fullGfData = null;
            var fullGfPath = modPrefix + '_meta/fullgameflow.json5';
            for (var fi = 0; fi < mappedFiles.length; fi++) {
                if (mappedFiles[fi].path === fullGfPath) {
                    fullGfData = mappedFiles[fi].data;
                    break;
                }
            }
            if (fullGfData) {
                var fullText = new TextDecoder().decode(fullGfData);
                var fullGf = _trxParseJSON5(fullText);
                // Build set of uploaded level filenames
                var uploadedLevels = {};
                for (var fi = 0; fi < mappedFiles.length; fi++) {
                    var p = mappedFiles[fi].path;
                    if (p.indexOf(modPrefix + 'levels/') === 0) {
                        var fn = p.split('/').pop();
                        uploadedLevels[fn] = true;
                    }
                }
                // Filter to levels that have uploaded files
                var filteredLevels = [];
                var titleEntry = null;
                var fullLevels = fullGf.levels || [];
                for (var li = 0; li < fullLevels.length; li++) {
                    var src = fullLevels[li];
                    var lvlPath = (src.path || '').split('/').pop().toLowerCase();
                    if (!uploadedLevels[lvlPath]) continue;
                    // Build clean level entry with only TRX fields
                    var lvl = {
                        path: lvlPath,
                        music_track: src.music_track != null ? src.music_track : -1,
                        lara_outfit: src.lara_outfit || defaultOutfit,
                        sequence: src.sequence || [],
                    };
                    // Clean injection paths (strip directory prefixes)
                    if (src.injections) {
                        lvl.injections = src.injections.map(function(p) {
                            return p.split('/').pop();
                        });
                    }
                    // Title/gym level → set as root-level title
                    if (src.type === 'gym_home' || lvlPath === 'title.tr2') {
                        titleEntry = {
                            path: lvlPath,
                            music_track: -1,
                            sequence: [
                                { type: 'exit_to_title' },
                            ],
                        };
                    } else {
                        filteredLevels.push(lvl);
                    }
                }
                if (filteredLevels.length > 0) {
                    // Read template for base fields
                    var tplText = new TextDecoder().decode(
                        FS.readFile('/games/' + templateMod + '/gameflow.json5'));
                    var tplGf = _trxParseJSON5(tplText);
                    tplGf.levels = filteredLevels;
                    // Use the full gameflow's root title section if
                    // available — it has the correct music_track.
                    if (fullGf.title) {
                        var ft = fullGf.title;
                        titleEntry = {
                            path: (ft.path || '').split('/').pop().toLowerCase(),
                            music_track: ft.music_track != null ? ft.music_track : -1,
                            sequence: [{ type: 'exit_to_title' }],
                        };
                    }
                    if (titleEntry) {
                        tplGf.title = titleEntry;
                    }
                    // Keep the template's global injections — they
                    // include TRX-required files like lara_outfits.bin
                    // that the TR2X gameflow doesn't have. Per-level
                    // injections (music_tracks, textures) are already
                    // in each level entry.
                    var gfJson = JSON.stringify(tplGf, null, 4);
                    var gfBytes = new TextEncoder().encode(gfJson);
                    _trxWriteToVFS(gfEntry.path, gfBytes);
                    gfEntry.data = gfBytes;
                    // Always regenerate strings — the TR2X template
                    // strings don't match our filtered level count.
                    {
                        _trxGenerateStrings(mappedFiles, modDir, templateMod,
                            filteredLevels.map(function(l) {
                                var p = l.path || '';
                                return { title: p.replace(/\.[^.]+$/, '') };
                            }));
                    }
                    return;
                }
            }
            // No full gameflow or no matching levels — fall through
            // to auto-generation
            hasGameflow = false;
            hasStrings = false;
        }
    }

    if (hasGameflow) {
        var gameflow = _trxParseJSON5(gfText);

        // Patch missing fields required by the engine.
        var gfDirty = false;
        if (!gameflow.main_menu_picture) {
            var tplText = new TextDecoder().decode(
                FS.readFile('/games/' + templateMod + '/gameflow.json5'));
            var tplGf = _trxParseJSON5(tplText);
            gameflow.main_menu_picture = tplGf.main_menu_picture;
            gfDirty = true;
        }
        var levels = gameflow.levels || [];
        for (var li = 0; li < levels.length; li++) {
            if (levels[li].type === 'title'
                || levels[li].type === 'dummy'
                || levels[li].type === 'current') continue;
            if (!levels[li].lara_outfit) {
                levels[li].lara_outfit = 'tr2_classic';
                gfDirty = true;
            }
            // Strip injection directory prefixes from TR2X/TR1X
            // uploads (e.g. "data/injections/font.bin" → "font.bin").
            if (levels[li].injections) {
                var inj = levels[li].injections;
                for (var ii = 0; ii < inj.length; ii++) {
                    var stripped = inj[ii].split('/').pop();
                    if (stripped !== inj[ii]) {
                        inj[ii] = stripped;
                        gfDirty = true;
                    }
                }
            }
        }
        // Also strip global injections
        if (gameflow.injections) {
            for (var ii = 0; ii < gameflow.injections.length; ii++) {
                var stripped = gameflow.injections[ii].split('/').pop();
                if (stripped !== gameflow.injections[ii]) {
                    gameflow.injections[ii] = stripped;
                    gfDirty = true;
                }
            }
        }
        if (gfDirty) {
            var gfJson = JSON.stringify(gameflow, null, 4);
            var gfBytes = new TextEncoder().encode(gfJson);
            _trxWriteToVFS(gfEntry.path, gfBytes);
            gfEntry.data = gfBytes;
        }

        if (!hasStrings) {
            var levels = gameflow.levels || [];
            _trxGenerateStrings(mappedFiles, modDir, templateMod,
                levels.map(function(l) {
                    var p = l.path || '';
                    return { title: p.replace(/\.[^.]+$/, '') };
                }));
        }
        return;
    }

    // --- Generate from TOMBPC.DAT (classic TR2/TR3 game script) ---
    if (scriptEntry
        && (templateMod === 'tr2-level' || templateMod === 'tr3-level')) {
        try {
            var scriptInfo = _trxParseTR2Script(scriptEntry.data);
            if (scriptInfo && scriptInfo.filenames.length > 1) {
                var tplText = new TextDecoder().decode(
                    FS.readFile('/games/' + templateMod
                                + '/gameflow.json5'));
                var gameflow = _trxParseJSON5(tplText);
                var templateLevel = gameflow.levels[0];

                // Detect uploaded title file (may differ from
                // script name).
                var levelPrefix = modPrefix + 'levels/';
                var uploadedTitle = null;
                for (var i = 0; i < mappedFiles.length; i++) {
                    if (mappedFiles[i].path
                            .indexOf(levelPrefix) === 0) {
                        var bn = mappedFiles[i].path
                            .substring(levelPrefix.length)
                            .replace(/\.[^.]+$/, '').toLowerCase();
                        if (bn === 'title') {
                            uploadedTitle = mappedFiles[i].path
                                .substring(levelPrefix.length);
                        }
                    }
                }

                // Title entry (level 0 in script).
                var scriptTitle = scriptInfo.filenames[0]
                    .replace(/\\/g, '/').split('/').pop()
                    .toLowerCase();
                gameflow.title = {
                    path: uploadedTitle || scriptTitle,
                    music_track: scriptInfo.titleSoundId,
                    sequence: [{ type: 'exit_to_title' }],
                };

                // Playable levels (1..n in script order).
                gameflow.levels = [];
                var titleEntries = [];
                for (var i = 1; i < scriptInfo.filenames.length;
                     i++) {
                    var base = scriptInfo.filenames[i]
                        .replace(/\\/g, '/').split('/').pop()
                        .toLowerCase();
                    var entry = JSON.parse(
                        JSON.stringify(templateLevel));
                    entry.path = base;
                    entry.lara_outfit = defaultOutfit;
                    entry.music_track =
                        scriptInfo.musicTracks[i];
                    // Use translated sequence if available,
                    // otherwise keep the template default.
                    if (scriptInfo.sequences[i]) {
                        entry.sequence = scriptInfo.sequences[i];
                    }
                    gameflow.levels.push(entry);

                    var title =
                        (i < scriptInfo.levelTitles.length
                         && scriptInfo.levelTitles[i])
                            ? scriptInfo.levelTitles[i]
                            : base.replace(/\.[^.]+$/, '');
                    titleEntries.push({ title: title });
                }

                var gfJson = JSON.stringify(gameflow, null, 4);
                var gfData = new TextEncoder().encode(gfJson);
                var gfPath = modPrefix + 'gameflow.json5';
                _trxWriteToVFS(gfPath, gfData);
                mappedFiles.push(
                    { path: gfPath, data: gfData });

                _trxGenerateStrings(mappedFiles, modDir,
                    templateMod, titleEntries);

                // Rename music files to CD track numbering so
                // they match trigger values in the level data.
                if (scriptInfo.cdOffset !== 0) {
                    var mPrefix = modPrefix + 'music/';
                    for (var i = 0; i < mappedFiles.length;
                         i++) {
                        var mp = mappedFiles[i].path;
                        if (mp.indexOf(mPrefix) !== 0) continue;
                        var fn = mp.substring(mPrefix.length);
                        var m = fn.match(/^(\d+)(\..*)/);
                        if (!m) continue;
                        var cdName = mPrefix
                            + (parseInt(m[1])
                               + scriptInfo.cdOffset) + m[2];
                        FS.writeFile('/' + cdName,
                            FS.readFile('/' + mp));
                        try { FS.unlink('/' + mp); } catch(e) {}
                        mappedFiles[i].path = cdName;
                    }
                }
                return;
            }
        } catch (e) {
            console.warn('[TRX] TOMBPC.DAT processing failed, '
                + 'falling back to auto-generation:', e);
        }
    }

    // --- Auto-generate gameflow from template (fallback) ---
    var tplText = new TextDecoder().decode(
        FS.readFile('/games/' + templateMod + '/gameflow.json5'));
    var gameflow = _trxParseJSON5(tplText);
    var templateLevel = gameflow.levels[0];

    // Find level files, separating title screen from playable levels.
    var levelPrefix = modPrefix + 'levels/';
    var levelFiles = [];
    var titleFile = null;
    for (var i = 0; i < mappedFiles.length; i++) {
        if (mappedFiles[i].path.indexOf(levelPrefix) === 0) {
            var filename = mappedFiles[i].path.substring(levelPrefix.length);
            var baseName = filename.replace(/\.[^.]+$/, '').toLowerCase();
            if (baseName === 'title') {
                titleFile = filename;
            } else {
                levelFiles.push(filename);
            }
        }
    }
    levelFiles.sort();

    // Add title entry (required by engine).  Use the dedicated title
    // file if present, otherwise fall back to the first playable level.
    gameflow.title = {
        path: titleFile || levelFiles[0] || 'PLACEHOLDER',
        music_track: -1,
        sequence: [
            { type: 'exit_to_title' },
        ],
    };

    // Generate one level entry per file (cloned from template)
    gameflow.levels = levelFiles.map(function(filename) {
        var entry = JSON.parse(JSON.stringify(templateLevel));
        entry.path = filename;
        entry.lara_outfit = defaultOutfit;
        return entry;
    });

    // Write generated gameflow
    var gameflowJson = JSON.stringify(gameflow, null, 4);
    var gameflowData = new TextEncoder().encode(gameflowJson);
    var gameflowPath = modPrefix + 'gameflow.json5';
    _trxWriteToVFS(gameflowPath, gameflowData);
    mappedFiles.push({ path: gameflowPath, data: gameflowData });

    // Generate strings files with level titles matching the gameflow.
    _trxGenerateStrings(mappedFiles, modDir, templateMod,
        levelFiles.map(function(f) {
            return { title: f.replace(/\.[^.]+$/, '') };
        }));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Inject a 'level_default' outfit into outfits.json5 on the VFS so
// that custom levels can use the Lara meshes baked into the level
// file.  Also injects the display string into base_strings files.
// The modified files are written to the mod directory so they take
// priority over the common config without altering the originals.
function _trxInjectLevelDefaultOutfit(modDir, templateMod) {
    // Engine-specific braid and gun_map settings.
    var isTR1 = templateMod === 'tr1-level';
    var braid;
    if (isTR1) {
        braid = {
            mode: 'BRAID_MODE_TR1_FULL',
            mesh_offset: 10,
            gold_offset: 16,
            hair_pos: { x: 0, y: 20, z: -45 },
        };
    } else {
        braid = {
            mesh_offset: 22,
            gold_offset: 28,
            hair_pos: { x: 0, y: -23, z: -55 },
        };
    }
    var gunMap = isTR1 ? 0 : (templateMod === 'tr3-level' ? 3 : 2);

    try {
        var outfitsText = new TextDecoder().decode(
            FS.readFile('/cfg/outfits.json5'));
        var outfits = _trxParseJSON5(outfitsText);
        if (outfits.outfits && !outfits.outfits.level_default) {
            outfits.outfits.level_default = {
                name_gs: 'dynamic/enums/lara_outfit/level_default',
                mesh_object: 'O_LARA',
                gun_map: gunMap,
                combat_face_offset: -1,
                supports_sunglasses: false,
                braid: braid,
            };
            _trxWriteToVFS('games/' + modDir + '/outfits.json5',
                new TextEncoder().encode(
                    JSON.stringify(outfits, null, 4)));
        }
    } catch (e) {
        console.warn('[TRX] Failed to inject level_default outfit:', e);
    }

    try {
        var cfgEntries = FS.readdir('/cfg');
        for (var i = 0; i < cfgEntries.length; i++) {
            if (cfgEntries[i].indexOf('base_strings') !== 0
                || !cfgEntries[i].endsWith('.json5')) continue;
            var strText = new TextDecoder().decode(
                FS.readFile('/cfg/' + cfgEntries[i]));
            var strObj = _trxParseJSON5(strText);
            if (strObj.dynamic && strObj.dynamic.enums
                && strObj.dynamic.enums.lara_outfit
                && !strObj.dynamic.enums.lara_outfit.level_default) {
                strObj.dynamic.enums.lara_outfit.level_default
                    = 'Level Default';
                _trxWriteToVFS(
                    'games/' + modDir + '/' + cfgEntries[i],
                    new TextEncoder().encode(
                        JSON.stringify(strObj, null, 4)));
            }
        }
    } catch (e) {
        console.warn('[TRX] Failed to inject level_default string:', e);
    }
}

// Generate strings files from template, replacing levels with the
// given titles array so the count matches the actual gameflow.
function _trxGenerateStrings(mappedFiles, modDir, templateMod, levelTitles) {
    try {
        var entries = FS.readdir('/games/' + templateMod);
        for (var i = 0; i < entries.length; i++) {
            if (entries[i].indexOf('strings') === 0
                && entries[i].endsWith('.json5')) {
                var tplStrText = new TextDecoder().decode(
                    FS.readFile('/games/' + templateMod + '/' + entries[i]));
                var strObj = _trxParseJSON5(tplStrText);
                strObj.levels = levelTitles;
                var strJson = JSON.stringify(strObj, null, 4);
                var strData = new TextEncoder().encode(strJson);
                var strPath = 'games/' + modDir + '/' + entries[i];
                FS.writeFile('/' + strPath, strData);
                mappedFiles.push({ path: strPath, data: strData });
            }
        }
    } catch(e) {}
}

// Write data to VFS, creating parent directories as needed.
function _trxWriteToVFS(path, data) {
    var parts = path.split('/');
    var dir = '';
    for (var i = 0; i < parts.length - 1; i++) {
        dir += (dir ? '/' : '') + parts[i];
        try { FS.mkdir('/' + dir); } catch(e) {}
    }
    FS.writeFile('/' + path, data);
}
