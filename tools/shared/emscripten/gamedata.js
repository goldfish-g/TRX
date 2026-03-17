// Game data manager for TRX WebGL builds.
//
// Handles uploading, extracting, and mapping user game files into the
// games/<mod>/ VFS directory structure.  Works with the ProfileManager
// to persist data in IndexedDB per-profile.
//
// Depends on: fflate (UMD, expected as global `fflate`)
//             Emscripten FS  (global `FS`)
//             Emscripten Module (global `Module`)

'use strict';

// ---------------------------------------------------------------------------
// GameDataManager
// ---------------------------------------------------------------------------

var GameDataManager = (function () {
    // Game file extensions we care about, grouped by destination directory.
    var LEVEL_EXTS = ['.phd', '.tr2', '.psx', '.tub'];
    var MUSIC_EXTS = ['.flac', '.ogg', '.mp3', '.wav'];
    var SFX_EXTS   = ['.sfx'];
    var FMV_EXTS   = ['.mp4', '.rpl', '.ogv', '.avi', '.fmv'];
    var AUDIO_EXTS = ['.wad'];  // cdaudio.wad for TR3
    var CUT_EXTS   = ['.tr2'];  // cutscene files live in cuts/

    // Remaster-only extensions to skip (textures, models, palettes, etc.).
    var REMASTER_SKIP_EXTS = ['.trg', '.dds', '.trm', '.pdp', '.map', '.tex'];

    // Known directory names (case-insensitive) that indicate the game root.
    var ROOT_MARKERS = ['data', 'fmv', 'music', 'audio', 'cuts', 'tracks', 'sfx'];

    // -----------------------------------------------------------------------
    // Constructor
    // -----------------------------------------------------------------------

    function GameDataManager(modId) {
        this.modId = modId;  // "tr1", "tr1-ub", "tr2", "tr2-gm", "tr3", "tr3-la", etc.
    }

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    // Process uploaded files (from file input or drag-and-drop).
    // Returns an array of { path: 'games/<mod>/...', data: Uint8Array }.
    // `onProgress(message, fraction)` for status updates.
    // `onWarning(message)` when no FMV cutscenes found; returns Promise.
    // `onLanguageSelect(languages)` for multi-language audio; returns Promise.
    GameDataManager.prototype.processUpload = function (items, onProgress, onWarning, onLanguageSelect) {
        var self = this;

        function report(msg, frac) {
            if (onProgress) onProgress(msg, frac);
        }

        // Read all files depending on source type.
        return _readUploadedItems(items, report).then(function (entries) {
            if (entries.length === 0) {
                throw new Error('No game files found in the upload.');
            }

            // Detect multiple audio language directories (e.g. SFX/DE/,
            // SFX/FR/) and let the user pick before mapping.
            var languages = _detectAudioLanguages(entries);
            var langStep;
            if (languages.length > 1 && onLanguageSelect) {
                report('Selecting audio language...', 0.65);
                langStep = onLanguageSelect(languages).then(function (lang) {
                    return _filterByLanguage(entries, lang);
                });
            } else {
                langStep = Promise.resolve(entries);
            }

            return langStep.then(function (filteredEntries) {

            report('Mapping files...', 0.7);
            var mapped = _mapFiles(filteredEntries, self.modId);
            if (mapped.length === 0) {
                throw new Error('No recognised game files found. Please provide your original Tomb Raider game files.');
            }

            // Fix remastered MAIN.SFX (has an index table prepended).
            for (var i = 0; i < mapped.length; i++) {
                if (_hasExt(_getExt(mapped[i].path), SFX_EXTS)) {
                    mapped[i].data = _stripRemasteredSFXHeader(mapped[i].data);
                }
            }

            // Warn the user if no FMV cutscenes were found.
            var hasFMV = false;
            for (var i = 0; i < mapped.length; i++) {
                if (_isFMV(mapped[i].path)) { hasFMV = true; break; }
            }

            var proceed = hasFMV || !onWarning
                ? Promise.resolve()
                : onWarning(
                    'No FMV videos (.rpl, .ogv, .mp4, .avi) were found. '
                    + 'The game will work, but FMVs will be skipped.'
                );

            return proceed.then(function () {
                report('Done mapping.', 0.75);
                return mapped;
            });

            }); // langStep.then
        });
    };

    // Write mapped files into the Emscripten FS.
    GameDataManager.prototype.loadMappedToFS = function (mappedFiles) {
        for (var i = 0; i < mappedFiles.length; i++) {
            _writeToFS(mappedFiles[i].path, mappedFiles[i].data);
        }
    };

    // -----------------------------------------------------------------------
    // File reading (from uploads)
    // -----------------------------------------------------------------------

    function _readUploadedItems(items, report) {
        // Convert to plain array of Files
        var files = [];
        for (var i = 0; i < items.length; i++) {
            files.push(items[i]);
        }

        if (files.length === 0) {
            return Promise.resolve([]);
        }

        // Single file: might be an archive
        if (files.length === 1 && !files[0].webkitRelativePath) {
            var file = files[0];
            var name = file.name.toLowerCase();
            if (name.endsWith('.zip') || name.endsWith('.tar.gz') || name.endsWith('.tgz')) {
                report('Reading archive...', 0.1);
                return _readFileAsArrayBuffer(file).then(function (buf) {
                    if (name.endsWith('.zip')) {
                        report('Extracting ZIP...', 0.2);
                        return _extractZip(new Uint8Array(buf), report);
                    } else {
                        report('Extracting tar.gz...', 0.2);
                        return _extractTarGz(new Uint8Array(buf), report);
                    }
                });
            }
        }

        // Folder upload or multiple files — read each file
        report('Reading files...', 0.1);
        var entries = [];
        var loaded = 0;

        function readNext() {
            if (loaded >= files.length) return Promise.resolve(entries);
            var f = files[loaded];
            var path = f.webkitRelativePath || f.name;
            // Android returns URL-encoded content-URI paths in webkitRelativePath
            // (e.g. "primary%3adownload%2ftr%20data%2f...").  Decode so that
            // %2f becomes "/" and root detection / path splitting works.
            try { path = decodeURIComponent(path); } catch (e) { /* already plain */ }
            return _readFileAsArrayBuffer(f).then(function (buf) {
                entries.push({ path: path, data: new Uint8Array(buf) });
                loaded++;
                report('Reading ' + path + '...', 0.1 + 0.5 * (loaded / files.length));
                return readNext();
            });
        }

        return readNext();
    }

    function _readFileAsArrayBuffer(file) {
        return new Promise(function (resolve, reject) {
            var reader = new FileReader();
            reader.onload = function () { resolve(reader.result); };
            reader.onerror = function () { reject(reader.error); };
            reader.readAsArrayBuffer(file);
        });
    }

    // -----------------------------------------------------------------------
    // Archive extraction
    // -----------------------------------------------------------------------

    function _extractZip(data, report) {
        var entries = [];
        var decompressed = fflate.unzipSync(data);
        var keys = Object.keys(decompressed);
        for (var i = 0; i < keys.length; i++) {
            var path = keys[i];
            // Skip directories (they end with /)
            if (path.endsWith('/')) continue;
            entries.push({ path: path, data: decompressed[path] });
            if (report && i % 50 === 0) {
                report('Extracting ' + path + '...', 0.2 + 0.5 * (i / keys.length));
            }
        }
        return entries;
    }

    function _extractTarGz(data, report) {
        // Decompress gzip first
        if (report) report('Decompressing gzip...', 0.2);
        var tarData = fflate.gunzipSync(data);

        // Parse tar
        if (report) report('Parsing tar archive...', 0.4);
        return _parseTar(tarData, report);
    }

    // Minimal tar parser (POSIX ustar / GNU tar).
    function _parseTar(data, report) {
        var entries = [];
        var offset = 0;

        while (offset + 512 <= data.length) {
            // Check for end-of-archive (two zero blocks)
            var allZero = true;
            for (var i = 0; i < 512; i++) {
                if (data[offset + i] !== 0) { allZero = false; break; }
            }
            if (allZero) break;

            // Parse header
            var name = _tarString(data, offset, 100);
            var size = _tarOctal(data, offset + 124, 12);
            var typeFlag = data[offset + 156];
            var prefix = _tarString(data, offset + 345, 155);

            if (prefix) name = prefix + '/' + name;

            offset += 512; // skip header

            // Type 0 or ASCII '0' = regular file
            if ((typeFlag === 0 || typeFlag === 48) && size > 0) {
                var fileData = data.slice(offset, offset + size);
                entries.push({ path: name, data: fileData });
            }

            // Advance past file data (rounded up to 512-byte blocks)
            offset += Math.ceil(size / 512) * 512;

            if (report && entries.length % 50 === 0) {
                report('Extracting ' + name + '...', 0.4 + 0.3 * (offset / data.length));
            }
        }

        return entries;
    }

    function _tarString(data, offset, len) {
        var s = '';
        for (var i = 0; i < len; i++) {
            if (data[offset + i] === 0) break;
            s += String.fromCharCode(data[offset + i]);
        }
        return s;
    }

    function _tarOctal(data, offset, len) {
        var s = _tarString(data, offset, len).trim();
        return parseInt(s, 8) || 0;
    }

    // -----------------------------------------------------------------------
    // File mapping / normalisation
    // -----------------------------------------------------------------------

    // Map raw extracted entries to VFS paths the engine expects.
    function _mapFiles(entries, modId) {
        // 1. Find the root by looking for known directory patterns.
        var root = _detectRoot(entries);

        // 2. Strip root prefix, normalise case, and classify each file.
        var mapped = [];
        for (var i = 0; i < entries.length; i++) {
            var rawPath = entries[i].path;
            var data = entries[i].data;

            // Strip detected root prefix
            var relPath = rawPath;
            if (root && relPath.toLowerCase().indexOf(root.toLowerCase()) === 0) {
                relPath = relPath.substring(root.length);
            }

            // Normalise separators and case
            relPath = relPath.replace(/\\/g, '/').replace(/^\/+/, '');
            var lowerPath = relPath.toLowerCase();

            // Skip empty or irrelevant files
            if (!relPath || data.byteLength === 0) continue;

            // Classify and map to VFS path
            var vfsPath = _classifyFile(lowerPath, relPath, modId);
            if (vfsPath) {
                mapped.push({ path: vfsPath, data: data });
            }
        }

        return mapped;
    }

    // Detect the root directory prefix by looking for common game directory
    // names (data/, fmv/, music/, etc.) in the entry paths.
    function _detectRoot(entries) {
        // Count occurrences of potential roots
        var candidates = {};
        for (var i = 0; i < entries.length; i++) {
            var path = entries[i].path.replace(/\\/g, '/');
            var parts = path.toLowerCase().split('/');
            for (var j = 0; j < parts.length - 1; j++) {
                for (var k = 0; k < ROOT_MARKERS.length; k++) {
                    if (parts[j] === ROOT_MARKERS[k]) {
                        // The root is everything before this directory
                        var root = path.split('/').slice(0, j).join('/');
                        if (root) root += '/';
                        candidates[root] = (candidates[root] || 0) + 1;
                    }
                }
            }
        }

        // Pick the most common root (or empty string if files are at top level)
        var bestRoot = '';
        var bestCount = 0;
        var keys = Object.keys(candidates);
        for (var i = 0; i < keys.length; i++) {
            if (candidates[keys[i]] > bestCount) {
                bestCount = candidates[keys[i]];
                bestRoot = keys[i];
            }
        }

        return bestRoot;
    }

    // Classify a file by its path and extension, returning the VFS path the
    // engine expects under games/<mod>/, or null if the file should be skipped.
    function _classifyFile(lowerPath, originalRelPath, modId) {
        var ext = _getExt(lowerPath);
        var lowerBase = lowerPath.split('/').pop();
        var prefix = 'games/' + modId + '/';

        // --- Skip remaster-only files (.trg, .dds, .trm, .pdp, .map, .tex) ---
        if (_hasExt(ext, REMASTER_SKIP_EXTS)) {
            return null;
        }

        // --- Level files (.phd for TR1, .tr2 for TR2/TR3) ---
        if (_hasExt(ext, LEVEL_EXTS)) {
            // TR3 cutscene files live in cuts/, identified by being in a
            // "cuts" directory or starting with "cut" prefix.
            var inCutsDir = lowerPath.indexOf('cuts/') === 0;
            if (inCutsDir) {
                return prefix + 'cuts/' + lowerBase;
            }
            return prefix + 'levels/' + lowerBase;
        }

        // --- Sound effects ---
        // Remastered stores SFX under sfx/ (e.g. sfx/main.sfx) instead
        // of data/.  Both layouts map to the mod root.
        if (_hasExt(ext, SFX_EXTS)) {
            return prefix + lowerBase;
        }

        // --- Music tracks ---
        // Remastered stores music under tracks/ (e.g. tracks/2.ogg)
        // instead of music/.  Both layouts map to music/.
        if (_hasExt(ext, MUSIC_EXTS)) {
            return prefix + 'music/' + lowerBase;
        }

        // --- FMV cutscenes (.rpl, .ogv, .mp4, .avi — decoded by FFmpeg) ---
        if (_hasExt(ext, FMV_EXTS)) {
            return prefix + 'fmv/' + lowerBase;
        }

        // --- Audio WAD (TR3) ---
        if (_hasExt(ext, AUDIO_EXTS)) {
            return prefix + 'audio/' + lowerBase;
        }

        // --- Config files (.json5) — gameflow, strings ---
        // Only map root-level json5 files (not inside subdirectories).
        if (ext === '.json5' && lowerPath.indexOf('/') === -1) {
            return prefix + lowerBase;
        }

        return null;  // skip unknown files
    }

    function _getExt(path) {
        var dot = path.lastIndexOf('.');
        return dot >= 0 ? path.substring(dot) : '';
    }

    function _hasExt(ext, list) {
        for (var i = 0; i < list.length; i++) {
            if (ext === list[i]) return true;
        }
        return false;
    }

    function _isFMV(path) {
        return _hasExt(_getExt(path.toLowerCase()), FMV_EXTS);
    }

    // The Remastered release prepends an index table to MAIN.SFX.  TRX
    // expects the file to start with concatenated RIFF/WAVE chunks, so
    // strip everything before the first RIFF signature.
    function _stripRemasteredSFXHeader(data) {
        // Already starts with RIFF — nothing to do.
        if (data.length >= 4
            && data[0] === 0x52 && data[1] === 0x49
            && data[2] === 0x46 && data[3] === 0x46) {
            return data;
        }
        // Scan for the first RIFF signature (aligned to 2-byte boundary).
        for (var i = 2; i <= data.length - 4; i += 2) {
            if (data[i]     === 0x52 && data[i + 1] === 0x49
                && data[i + 2] === 0x46 && data[i + 3] === 0x46) {
                console.log('[GDM] Stripping ' + i + '-byte remastered SFX header');
                return data.slice(i);
            }
        }
        // No RIFF found — return as-is and let the engine report the error.
        return data;
    }

    // -----------------------------------------------------------------------
    // Audio language detection / filtering
    // -----------------------------------------------------------------------

    // Language codes found in the remastered release's TRACKS/ directories.
    var LANG_NAMES = {
        de: 'German',   en: 'English',  es: 'Spanish',
        fr: 'French',   it: 'Italian',  ja: 'Japanese',
        ru: 'Russian'
    };

    // Scan uploaded entries for language-specific audio directories under
    // TRACKS/ (e.g. TRACKS/EN/22.OGG, TRACKS/RU/22.OGG).  Only languages
    // that have voiced dialogue tracks are offered.  Returns a sorted
    // array of { code, name } objects, or an empty array if fewer than
    // two language variants are found.
    function _detectAudioLanguages(entries) {
        var found = {};
        for (var i = 0; i < entries.length; i++) {
            var path = entries[i].path.replace(/\\/g, '/').toLowerCase();
            var m = path.match(/(?:^|\/)tracks\/([a-z]{2})\//);
            if (m) {
                found[m[1]] = true;
            }
        }
        var codes = Object.keys(found);
        if (codes.length <= 1) return [];
        codes.sort();
        return codes.map(function (c) {
            return { code: c, name: LANG_NAMES[c] || c.toUpperCase() };
        });
    }

    // Remove entries from non-selected language directories under SFX/
    // or TRACKS/.  Entries not inside any language subdirectory are kept.
    function _filterByLanguage(entries, lang) {
        var lowerLang = lang.toLowerCase();
        var filtered = [];
        for (var i = 0; i < entries.length; i++) {
            var path = entries[i].path.replace(/\\/g, '/').toLowerCase();
            var m = path.match(/(?:^|\/)(sfx|tracks)\/([a-z]{2})\//);
            if (m) {
                if (m[2] === lowerLang) {
                    filtered.push(entries[i]);
                }
            } else {
                filtered.push(entries[i]);
            }
        }
        return filtered;
    }

    // -----------------------------------------------------------------------
    // Emscripten FS helpers
    // -----------------------------------------------------------------------

    function _writeToFS(path, data) {
        // Ensure parent directories exist
        var parts = path.split('/');
        var dir = '';
        for (var i = 0; i < parts.length - 1; i++) {
            dir += (dir ? '/' : '') + parts[i];
            try { FS.mkdir('/' + dir); } catch (e) { /* already exists */ }
        }
        FS.writeFile('/' + path, data);
    }

    return GameDataManager;
})();
