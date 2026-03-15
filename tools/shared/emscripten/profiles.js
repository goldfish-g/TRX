// Profile manager for TRX WebGL builds.
//
// Manages multiple game profiles (TR1, TR2, TR3 and expansions) in a single
// unified build.  Each profile maps to a mod directory under games/ and stores
// user game data in IndexedDB so files only need to be provided once.
//
// Depends on: Emscripten FS (global `FS`)

'use strict';

// ---------------------------------------------------------------------------
// Mod definitions — maps mod ID to engine version and base mod
// ---------------------------------------------------------------------------

var MOD_DEFINITIONS = {
    'tr1':       { engine: 1, baseMod: null,  name: 'Tomb Raider I' },
    'tr1-ub':    { engine: 1, baseMod: 'tr1', name: 'Unfinished Business' },
    'tr1-level': { engine: 1, baseMod: 'tr1', name: 'TR1 Custom Level' },
    'tr2':       { engine: 2, baseMod: null,  name: 'Tomb Raider II' },
    'tr2-gm':    { engine: 2, baseMod: 'tr2', name: 'The Golden Mask' },
    'tr2-level': { engine: 2, baseMod: 'tr2', name: 'TR2 Custom Level' },
    'tr3':       { engine: 3, baseMod: null,  name: 'Tomb Raider III' },
    'tr3-la':    { engine: 3, baseMod: 'tr3', name: 'The Lost Artifact' },
    'tr3-level': { engine: 3, baseMod: 'tr3', name: 'TR3 Custom Level' },
};

// ---------------------------------------------------------------------------
// ProfileManager
// ---------------------------------------------------------------------------

var ProfileManager = (function () {
    var DB_NAME = 'trx-profiles';
    var DB_VERSION = 1;

    function ProfileManager() {
        this._db = null;
    }

    // -----------------------------------------------------------------------
    // IndexedDB setup
    // -----------------------------------------------------------------------

    ProfileManager.prototype._openDB = function () {
        var self = this;
        if (self._db) return Promise.resolve(self._db);
        return new Promise(function (resolve, reject) {
            var req = indexedDB.open(DB_NAME, DB_VERSION);
            req.onupgradeneeded = function () {
                var db = req.result;
                if (!db.objectStoreNames.contains('profiles')) {
                    db.createObjectStore('profiles', { keyPath: 'id' });
                }
                if (!db.objectStoreNames.contains('gamedata')) {
                    db.createObjectStore('gamedata');
                }
            };
            req.onsuccess = function () { self._db = req.result; resolve(self._db); };
            req.onerror = function () { reject(req.error); };
        });
    };

    // -----------------------------------------------------------------------
    // Profile CRUD
    // -----------------------------------------------------------------------

    ProfileManager.prototype.initDefaults = function (manifest) {
        var self = this;
        return self._openDB().then(function (db) {
            var promises = [];
            for (var i = 0; i < manifest.length; i++) {
                promises.push(self._ensureDefaultProfile(db, manifest[i]));
            }
            return Promise.all(promises);
        });
    };

    ProfileManager.prototype._ensureDefaultProfile = function (db, entry) {
        return new Promise(function (resolve, reject) {
            var tx = db.transaction('profiles', 'readonly');
            var req = tx.objectStore('profiles').get(entry.id);
            req.onsuccess = function () {
                var existing = req.result;
                var profile;
                if (existing) {
                    // Update mutable fields from the manifest on each load
                    // so that rebuilds with new dataPackage etc. take effect.
                    var changed = false;
                    if (existing.dataPackage !== (entry.dataPackage || null)) {
                        existing.dataPackage = entry.dataPackage || null;
                        changed = true;
                    }
                    if (existing.name !== entry.name) {
                        existing.name = entry.name;
                        changed = true;
                    }
                    if (existing.description !== (entry.description || '')) {
                        existing.description = entry.description || '';
                        changed = true;
                    }
                    if (!changed) { resolve(); return; }
                    profile = existing;
                } else {
                    profile = {
                        id: entry.id,
                        name: entry.name,
                        mod: entry.mod,
                        engine: entry.engine,
                        description: entry.description || '',
                        dataPackage: entry.dataPackage || null,
                        isDefault: true,
                        createdAt: new Date().toISOString(),
                        lastPlayed: null,
                    };
                }
                var writeTx = db.transaction('profiles', 'readwrite');
                writeTx.objectStore('profiles').put(profile);
                writeTx.oncomplete = resolve;
                writeTx.onerror = function () { reject(writeTx.error); };
            };
            req.onerror = function () { reject(req.error); };
        });
    };

    ProfileManager.prototype.listProfiles = function () {
        var self = this;
        return self._openDB().then(function (db) {
            return new Promise(function (resolve, reject) {
                var tx = db.transaction('profiles', 'readonly');
                var req = tx.objectStore('profiles').getAll();
                req.onsuccess = function () { resolve(req.result || []); };
                req.onerror = function () { reject(req.error); };
            });
        });
    };

    ProfileManager.prototype.getProfile = function (id) {
        var self = this;
        return self._openDB().then(function (db) {
            return new Promise(function (resolve, reject) {
                var tx = db.transaction('profiles', 'readonly');
                var req = tx.objectStore('profiles').get(id);
                req.onsuccess = function () { resolve(req.result || null); };
                req.onerror = function () { reject(req.error); };
            });
        });
    };

    ProfileManager.prototype.createProfile = function (opts) {
        var self = this;
        var modDef = MOD_DEFINITIONS[opts.mod];
        if (!modDef) return Promise.reject(new Error('Unknown mod: ' + opts.mod));

        var profile = {
            id: opts.id || (opts.mod + '-' + Date.now()),
            name: opts.name,
            mod: opts.mod,
            engine: modDef.engine,
            description: opts.description || '',
            isDefault: false,
            createdAt: new Date().toISOString(),
            lastPlayed: null,
        };

        return self._openDB().then(function (db) {
            return new Promise(function (resolve, reject) {
                var tx = db.transaction('profiles', 'readwrite');
                tx.objectStore('profiles').put(profile);
                tx.oncomplete = function () { resolve(profile); };
                tx.onerror = function () { reject(tx.error); };
            });
        });
    };

    ProfileManager.prototype.deleteProfile = function (id) {
        var self = this;
        return self._openDB().then(function (db) {
            // Delete profile record
            return new Promise(function (resolve, reject) {
                var tx = db.transaction('profiles', 'readwrite');
                tx.objectStore('profiles').delete(id);
                tx.oncomplete = resolve;
                tx.onerror = function () { reject(tx.error); };
            }).then(function () {
                // Delete associated game data
                return self._deleteGameData(db, id);
            });
        });
    };

    ProfileManager.prototype.updateLastPlayed = function (id) {
        var self = this;
        return self._openDB().then(function (db) {
            return new Promise(function (resolve, reject) {
                var tx = db.transaction('profiles', 'readwrite');
                var store = tx.objectStore('profiles');
                var req = store.get(id);
                req.onsuccess = function () {
                    if (req.result) {
                        req.result.lastPlayed = new Date().toISOString();
                        store.put(req.result);
                    }
                };
                tx.oncomplete = resolve;
                tx.onerror = function () { reject(tx.error); };
            });
        });
    };

    // -----------------------------------------------------------------------
    // Game data management
    // -----------------------------------------------------------------------

    // Check if user game data exists in IDB for a profile.
    ProfileManager.prototype.hasGameData = function (id) {
        var self = this;
        return self._openDB().then(function (db) {
            return new Promise(function (resolve, reject) {
                var prefix = id + '/';
                var tx = db.transaction('gamedata', 'readonly');
                var store = tx.objectStore('gamedata');
                var req = store.openCursor();
                req.onsuccess = function () {
                    var cursor = req.result;
                    if (cursor) {
                        if (typeof cursor.key === 'string' && cursor.key.indexOf(prefix) === 0) {
                            resolve(true);
                            return;
                        }
                        cursor.continue();
                    } else {
                        resolve(false);
                    }
                };
                req.onerror = function () { reject(req.error); };
            });
        });
    };

    // Store mapped files into IDB for a profile.
    // mappedFiles: array of { path: 'games/<mod>/...', data: Uint8Array }
    ProfileManager.prototype.storeGameData = function (id, mappedFiles, onProgress) {
        var self = this;
        return self._openDB().then(function (db) {
            var stored = 0;
            function storeNext() {
                if (stored >= mappedFiles.length) return Promise.resolve();
                var entry = mappedFiles[stored];
                var key = id + '/' + entry.path;
                return new Promise(function (resolve, reject) {
                    var tx = db.transaction('gamedata', 'readwrite');
                    tx.objectStore('gamedata').put(entry.data.buffer, key);
                    tx.oncomplete = resolve;
                    tx.onerror = function () { reject(tx.error); };
                }).then(function () {
                    stored++;
                    if (onProgress) onProgress(stored, mappedFiles.length);
                    return storeNext();
                });
            }
            return storeNext();
        });
    };

    // Load game data from IDB into the Emscripten VFS for a profile.
    ProfileManager.prototype.loadGameDataToFS = function (id, onProgress) {
        var self = this;
        return self._openDB().then(function (db) {
            var prefix = id + '/';
            return new Promise(function (resolve, reject) {
                var files = [];
                var tx = db.transaction('gamedata', 'readonly');
                var store = tx.objectStore('gamedata');
                var req = store.openCursor();
                req.onsuccess = function () {
                    var cursor = req.result;
                    if (cursor) {
                        if (typeof cursor.key === 'string' && cursor.key.indexOf(prefix) === 0) {
                            var vfsPath = cursor.key.substring(prefix.length);
                            files.push({ path: vfsPath, data: new Uint8Array(cursor.value) });
                        }
                        cursor.continue();
                    }
                };
                tx.oncomplete = function () { resolve(files); };
                tx.onerror = function () { reject(tx.error); };
            }).then(function (files) {
                for (var i = 0; i < files.length; i++) {
                    _writeToFS(files[i].path, files[i].data);
                    if (onProgress) onProgress(i + 1, files.length);
                }
            });
        });
    };

    // Check if the build preloaded game data for a given mod.
    ProfileManager.prototype.hasPreloadedGameData = function (mod) {
        try {
            var entries = FS.readdir('/games/' + mod + '/levels');
            for (var i = 0; i < entries.length; i++) {
                if (entries[i] === '.' || entries[i] === '..') continue;
                return true;
            }
        } catch (e) {
            // /games/<mod>/levels/ might not exist
        }
        return false;
    };

    // Cache preloaded game data from VFS into IDB for offline use.
    ProfileManager.prototype.dumpPreloadedToIDB = function (id, mod, onProgress) {
        var self = this;
        var GAME_EXTS = ['.phd', '.tr2', '.psx', '.tub', '.sfx',
                         '.flac', '.ogg', '.mp3', '.wav',
                         '.mp4', '.rpl', '.ogv', '.avi', '.fmv',
                         '.wad'];
        var files = [];

        function walk(dir) {
            var entries;
            try { entries = FS.readdir(dir); } catch (e) { return; }
            for (var i = 0; i < entries.length; i++) {
                if (entries[i] === '.' || entries[i] === '..') continue;
                var full = dir + '/' + entries[i];
                var stat;
                try { stat = FS.stat(full); } catch (e) { continue; }
                if (FS.isDir(stat.mode)) {
                    walk(full);
                } else {
                    var lower = entries[i].toLowerCase();
                    for (var j = 0; j < GAME_EXTS.length; j++) {
                        if (lower.endsWith(GAME_EXTS[j])) {
                            // Store path relative to / (e.g. "games/tr1/levels/gym.phd")
                            files.push(full.substring(1));
                            break;
                        }
                    }
                }
            }
        }

        var base = '/games/' + mod;
        walk(base + '/levels');
        walk(base + '/music');
        walk(base + '/fmv');
        walk(base + '/audio');
        walk(base + '/cuts');

        if (files.length === 0) return Promise.resolve();

        var mappedFiles = [];
        for (var i = 0; i < files.length; i++) {
            var data;
            try { data = FS.readFile('/' + files[i]); } catch (e) { continue; }
            mappedFiles.push({ path: files[i], data: data });
        }

        return self.storeGameData(id, mappedFiles, onProgress);
    };

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    ProfileManager.prototype._deleteGameData = function (db, id) {
        var prefix = id + '/';
        return new Promise(function (resolve, reject) {
            var tx = db.transaction('gamedata', 'readwrite');
            var store = tx.objectStore('gamedata');
            var req = store.openCursor();
            req.onsuccess = function () {
                var cursor = req.result;
                if (cursor) {
                    if (typeof cursor.key === 'string' && cursor.key.indexOf(prefix) === 0) {
                        cursor.delete();
                    }
                    cursor.continue();
                }
            };
            tx.oncomplete = resolve;
            tx.onerror = function () { reject(tx.error); };
        });
    };

    function _writeToFS(path, data) {
        var parts = path.split('/');
        var dir = '';
        for (var i = 0; i < parts.length - 1; i++) {
            dir += (dir ? '/' : '') + parts[i];
            try { FS.mkdir('/' + dir); } catch (e) { /* already exists */ }
        }
        FS.writeFile('/' + path, data);
    }

    // -----------------------------------------------------------------------
    // Bundled game data (tar packages)
    // -----------------------------------------------------------------------

    // Unpack a tar ArrayBuffer into an array of {path, data} entries.
    function _untarBuffer(buffer) {
        var view = new Uint8Array(buffer);
        var files = [];
        var offset = 0;
        while (offset + 512 <= view.length) {
            var header = view.subarray(offset, offset + 512);
            if (header[0] === 0) break;

            // File name (bytes 0-99)
            var name = '';
            for (var i = 0; i < 100 && header[i] !== 0; i++) {
                name += String.fromCharCode(header[i]);
            }

            // File size (bytes 124-135, octal ASCII)
            var sizeStr = '';
            for (var i = 124; i < 136 && header[i] !== 0 && header[i] !== 32; i++) {
                sizeStr += String.fromCharCode(header[i]);
            }
            var size = parseInt(sizeStr.trim(), 8) || 0;

            // Type flag (byte 156): '0' or null = regular file
            var type = header[156];

            offset += 512;

            if ((type === 48 || type === 0) && size > 0) {
                files.push({
                    path: name,
                    data: new Uint8Array(buffer, offset, size),
                });
            }

            offset += Math.ceil(size / 512) * 512;
        }
        return files;
    }

    // Load bundled game data from a tar package (or IDB cache) into the FS.
    // Checks IDB first; if not cached, fetches the tar and caches afterward.
    ProfileManager.prototype.loadBundledToFS = function (profileId, packageName, onProgress) {
        var self = this;

        return self.hasGameData(profileId).then(function (hasIDB) {
            if (hasIDB) {
                if (onProgress) onProgress('Loading cached data...', -1);
                return self.loadGameDataToFS(profileId);
            }

            // Fetch the tar package
            if (onProgress) onProgress('Downloading game data...', 0);
            var tarUrl = packageName + '.tar';
            if (typeof _trxCacheBust === 'string' && _trxCacheBust) {
                tarUrl += '?' + _trxCacheBust;
            }

            return fetch(tarUrl).then(function (resp) {
                if (!resp.ok) {
                    throw new Error('Failed to fetch ' + packageName + '.tar: ' + resp.status);
                }
                var total = parseInt(resp.headers.get('content-length') || '0', 10);
                var reader = resp.body.getReader();
                var received = 0;
                var chunks = [];

                function read() {
                    return reader.read().then(function (result) {
                        if (result.done) return;
                        chunks.push(result.value);
                        received += result.value.length;
                        if (onProgress && total > 0) {
                            onProgress('Downloading game data...', received / total);
                        }
                        return read();
                    });
                }

                return read().then(function () {
                    // Concatenate chunks into a single buffer
                    var data = new Uint8Array(received);
                    var pos = 0;
                    for (var i = 0; i < chunks.length; i++) {
                        data.set(chunks[i], pos);
                        pos += chunks[i].length;
                    }

                    if (onProgress) onProgress('Unpacking game data...', -1);
                    var files = _untarBuffer(data.buffer);

                    // Write to Emscripten FS
                    for (var i = 0; i < files.length; i++) {
                        _writeToFS(files[i].path, files[i].data);
                    }

                    // Cache to IDB in background for offline use
                    var mapped = [];
                    for (var i = 0; i < files.length; i++) {
                        mapped.push({ path: files[i].path, data: new Uint8Array(files[i].data) });
                    }
                    self.storeGameData(profileId, mapped).catch(function (err) {
                        console.warn('[TRX] Failed to cache game data to IDB:', err);
                    });
                });
            });
        });
    };

    // -----------------------------------------------------------------------
    // Reset default profiles
    // -----------------------------------------------------------------------

    // Clear cached game data for all default profiles and re-create any
    // deleted defaults from the manifest.  Save data and settings live in
    // a separate IDBFS database and are not affected.
    ProfileManager.prototype.resetDefaults = function (manifest) {
        var self = this;
        return self._openDB().then(function (db) {
            return new Promise(function (resolve, reject) {
                var tx = db.transaction('profiles', 'readonly');
                var req = tx.objectStore('profiles').getAll();
                req.onsuccess = function () { resolve(req.result || []); };
                req.onerror = function () { reject(req.error); };
            });
        }).then(function (profiles) {
            return profiles.filter(function (p) { return p.isDefault; })
                .reduce(function (chain, p) {
                    return chain.then(function () {
                        return self._openDB().then(function (db) {
                            return self._deleteGameData(db, p.id);
                        });
                    });
                }, Promise.resolve());
        }).then(function () {
            return self.initDefaults(manifest);
        });
    };

    return ProfileManager;
})();