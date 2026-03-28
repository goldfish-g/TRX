// Game startup, profile selector, and dialogs for TRX WebGL builds.
//
// Manages game launching, the profile selector overlay, and the
// create/delete/reset-defaults dialogs.
//
// Depends on: Emscripten Module      (global `Module`)
//             ProfileManager         (from profiles.js)
//             MOD_DEFINITIONS        (from profiles.js)
//             _trxShowUploadUI       (from upload.js)
//             _trxProfileManager, _trxCurrentProfile, _trxEngineStarting,
//             _trxDefaultManifest    (globals from shell.html)

'use strict';

// ---------------------------------------------------------------------------
// Available mods manifest
// ---------------------------------------------------------------------------

// Write a file to the Emscripten VFS listing all known profiles.
// The C mod system reads this synchronously during startup to know
// which profiles are available for "Switch Game" — including ones
// whose game data has not been downloaded yet.
function _trxWriteAvailableModsManifest(pm) {
    return pm.listProfiles().then(function (profiles) {
        var seen = {};
        var lines = [];
        for (var i = 0; i < profiles.length; i++) {
            var p = profiles[i];
            var mod = p.modDir || p.mod;
            if (!seen[mod]) {
                seen[mod] = true;
                var modDef = MOD_DEFINITIONS[p.mod] || {};
                var title = p.name || modDef.label || p.mod;
                var engine = p.engine || (modDef ? modDef.engine : 0);
                lines.push(mod + '|' + title + '|' + engine);
            }
        }
        try {
            FS.writeFile('/available_mods.txt', lines.join('\n') + '\n');
        } catch (e) {
            console.warn('[TRX] Failed to write available mods manifest:', e);
        }
    });
}

// ---------------------------------------------------------------------------
// Game startup
// ---------------------------------------------------------------------------

function _trxStartGame(profile) {
    _trxCurrentProfile = profile;
    _trxEngineStarting = true;
    var pm = _trxProfileManager;

    document.getElementById('loading').classList.remove('hidden');
    document.getElementById('profile-selector').classList.add('hidden');
    document.getElementById('upload-screen').classList.add('hidden');
    document.getElementById('status').textContent = 'Loading...';
    document.getElementById('progress-fill').style.width = '0%';

    var modDef = MOD_DEFINITIONS[profile.mod];
    var engine = modDef ? modDef.engine : profile.engine;

    // Load game data if needed (bundled tar or IDB cache)
    var dataReady;
    if (profile.dataPackage) {
        dataReady = pm.loadBundledToFS(profile.id, profile.dataPackage, function (msg, frac) {
            document.getElementById('status').textContent = msg;
            if (frac >= 0) {
                document.getElementById('progress-fill').style.width = Math.round(frac * 100) + '%';
            }
        });
    } else {
        dataReady = pm.hasGameData(profile.id).then(function (has) {
            if (has) {
                document.getElementById('status').textContent = 'Loading cached data...';
                return pm.loadGameDataToFS(profile.id, function (loaded, total) {
                    document.getElementById('progress-fill').style.width = Math.round((loaded / total) * 100) + '%';
                });
            }
        });
    }

    dataReady.then(function () {
        // Write available mods manifest to VFS before starting the engine.
        // The C mod system reads this to know which profiles have game data
        // (only one game's data is in the VFS at a time).
        return _trxWriteAvailableModsManifest(pm);
    }).then(function () {
        document.getElementById('progress-fill').style.width = '100%';
        document.getElementById('status').textContent = 'Starting...';
        document.getElementById('loading').classList.add('hidden');
        document.getElementById('canvas').style.display = '';
        document.getElementById('canvas').focus();
        if (typeof Module.syncViewportLayout === 'function') {
            Module.syncViewportLayout();
        }
        if (pm) pm.updateLastPlayed(profile.id).catch(function () {});
        var effectiveMod = profile.modDir || profile.mod;
        Module.callMain(['--engine', String(engine), '--mod', effectiveMod]);
    }).catch(function (err) {
        console.error('[TRX] Failed to load game data:', err);
        document.getElementById('status').textContent = 'Error: ' + (err.message || 'Failed to load game data');
    });
}

function _trxResumeWithProfile(profile) {
    // Called when returning from Exit Game via profile selector.
    // Load game data if needed, then signal C code.
    var pm = _trxProfileManager;
    _trxCurrentProfile = profile;
    var modDef = MOD_DEFINITIONS[profile.mod];
    var engine = modDef ? modDef.engine : profile.engine;

    document.getElementById('profile-selector').classList.add('hidden');
    document.getElementById('loading').classList.remove('hidden');
    document.getElementById('status').textContent = 'Loading...';
    document.getElementById('progress-fill').style.width = '0%';

    var dataReady;
    if (profile.dataPackage) {
        dataReady = pm.loadBundledToFS(profile.id, profile.dataPackage, function (msg, frac) {
            document.getElementById('status').textContent = msg;
            if (frac >= 0) {
                document.getElementById('progress-fill').style.width = Math.round(frac * 100) + '%';
            }
        });
    } else {
        dataReady = pm.hasGameData(profile.id).then(function (has) {
            if (has) return pm.loadGameDataToFS(profile.id);
        });
    }

    dataReady.then(function () {
        document.getElementById('loading').classList.add('hidden');
        document.getElementById('canvas').style.display = '';
        if (pm) pm.updateLastPlayed(profile.id).catch(function () {});
        var effectiveMod = profile.modDir || profile.mod;
        if (Module._profileCallback) {
            Module._profileCallback(effectiveMod, engine);
        }
    }).catch(function (err) {
        console.error('[TRX] Failed to load profile data:', err);
        document.getElementById('loading').classList.add('hidden');
        document.getElementById('canvas').style.display = '';
        var effectiveMod = profile.modDir || profile.mod;
        if (Module._profileCallback) {
            Module._profileCallback(effectiveMod, engine);
        }
    });
}

// Load a mod's game data into the VFS.  Called from C when switching
// mods via the in-game passport (not the profile selector).
// If the data is already cached in IDB it is loaded directly;
// otherwise the bundled data package is downloaded with a progress bar.
Module.loadModData = function (modName, callback) {
    var pm = _trxProfileManager;
    if (!pm) {
        callback();
        return;
    }
    pm.listProfiles().then(function (profiles) {
        // Find the profile whose effective mod matches
        for (var i = 0; i < profiles.length; i++) {
            var p = profiles[i];
            var effectiveMod = p.modDir || p.mod;
            if (effectiveMod === modName) {
                _trxCurrentProfile = p;
                return pm.hasGameData(p.id).then(function (has) {
                    if (has) {
                        return pm.loadGameDataToFS(p.id);
                    }
                    if (p.dataPackage) {
                        document.getElementById('loading').classList.remove('hidden');
                        document.getElementById('canvas').style.display = 'none';
                        document.getElementById('status').textContent = 'Downloading...';
                        document.getElementById('progress-fill').style.width = '0%';
                        return pm.loadBundledToFS(p.id, p.dataPackage, function (msg, frac) {
                            document.getElementById('status').textContent = msg;
                            if (frac >= 0) {
                                document.getElementById('progress-fill').style.width = Math.round(frac * 100) + '%';
                            }
                        }).then(function () {
                            document.getElementById('loading').classList.add('hidden');
                            document.getElementById('canvas').style.display = '';
                        });
                    }
                });
            }
        }
    }).then(function () {
        callback();
    }).catch(function (err) {
        console.error('[TRX] loadModData failed:', err);
        callback();
    });
};

// Show profile selector overlay. Used on initial load and when
// returning from Exit Game (called from C via EM_JS).
Module.showProfileSelector = function (callback) {
    Module._profileCallback = callback;
    _trxEngineStarting = false;
    if (Module.setTouchControlsVisible) {
        Module.setTouchControlsVisible(false);
    }
    _trxShowProfileSelector(callback);
};

// ---------------------------------------------------------------------------
// Profile selector
// ---------------------------------------------------------------------------

function _trxShowProfileSelector(callback) {
    var selector = document.getElementById('profile-selector');
    var listEl = document.getElementById('profile-list');
    var pm = _trxProfileManager;

    selector.classList.remove('hidden');
    document.getElementById('canvas').style.display = 'none';

    function refresh() {
        pm.listProfiles().then(function (profiles) {
            // Sort: defaults first (by engine), then user profiles
            profiles.sort(function (a, b) {
                if (a.isDefault !== b.isDefault) return a.isDefault ? -1 : 1;
                return (a.engine || 0) - (b.engine || 0);
            });
            _trxRenderProfileList(profiles, listEl, callback, refresh);
        });
    }

    refresh();

    // Create profile button
    var createBtn = document.getElementById('btn-create-profile');
    // Remove old listener by cloning
    var newBtn = createBtn.cloneNode(true);
    createBtn.parentNode.replaceChild(newBtn, createBtn);
    newBtn.addEventListener('click', function () {
        _trxShowCreateProfileDialog(function (profile) {
            refresh();
        });
    });

    // Reset defaults button
    var resetBtn = document.getElementById('btn-reset-defaults');
    var newResetBtn = resetBtn.cloneNode(true);
    resetBtn.parentNode.replaceChild(newResetBtn, resetBtn);
    newResetBtn.addEventListener('click', function () {
        _trxShowResetDefaultsDialog();
    });
}

function _trxRenderProfileList(profiles, listEl, callback, refresh) {
    var pm = _trxProfileManager;
    listEl.innerHTML = '';

    for (var i = 0; i < profiles.length; i++) {
        (function (profile) {
            var card = document.createElement('div');
            card.className = 'profile-card';

            var modDef = MOD_DEFINITIONS[profile.mod] || {};
            var engineLabel = 'TR' + (profile.engine || '?');

            var info = document.createElement('div');
            info.className = 'profile-info';

            var nameRow = document.createElement('div');
            nameRow.className = 'profile-name-row';

            var badge = document.createElement('span');
            badge.className = 'profile-badge';
            badge.textContent = engineLabel;
            nameRow.appendChild(badge);

            var nameEl = document.createElement('span');
            nameEl.className = 'profile-name';
            nameEl.textContent = profile.name;
            nameRow.appendChild(nameEl);

            info.appendChild(nameRow);

            if (profile.description) {
                var desc = document.createElement('div');
                desc.className = 'profile-desc';
                desc.textContent = profile.description;
                info.appendChild(desc);
            }

            card.appendChild(info);

            // Clicking the card launches the game.
            card.addEventListener('click', function () {
                // Show loading screen immediately for responsiveness.
                document.getElementById('profile-selector').classList.add('hidden');
                document.getElementById('loading').classList.remove('hidden');
                document.getElementById('status').textContent = 'Loading...';
                document.getElementById('progress-fill').style.width = '0%';

                pm.hasGameData(profile.id).then(function (hasIDB) {
                    if (hasIDB || profile.dataPackage) {
                        if (callback) {
                            _trxResumeWithProfile(profile);
                        } else {
                            _trxStartGame(profile);
                        }
                    } else {
                        // No game data — show upload UI
                        document.getElementById('loading').classList.add('hidden');
                        _trxShowUploadUI(profile, callback, refresh);
                    }
                });
            });

            var actions = document.createElement('div');
            actions.className = 'profile-card-actions';

            var deleteBtn = document.createElement('button');
            deleteBtn.type = 'button';
            deleteBtn.className = 'profile-delete';
            deleteBtn.textContent = 'Delete';
            deleteBtn.addEventListener('click', function (e) {
                e.stopPropagation();
                _trxShowDeleteDialog(profile, refresh);
            });
            actions.appendChild(deleteBtn);

            card.appendChild(actions);
            listEl.appendChild(card);
        })(profiles[i]);
    }

    if (profiles.length === 0) {
        var empty = document.createElement('p');
        empty.className = 'profile-empty';
        empty.textContent = 'No profiles yet. Create one to get started.';
        listEl.appendChild(empty);
    }
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

// Game options per engine version for the create-profile dialog.
var _trxGamesByEngine = {
    '1': [
        { mod: 'tr1',       label: 'Original Game' },
        { mod: 'tr1-ub',    label: 'Unfinished Business' },
        { mod: 'tr1-level', label: 'Custom Level' },
    ],
    '2': [
        { mod: 'tr2',       label: 'Original Game' },
        { mod: 'tr2-gm',    label: 'The Golden Mask' },
        { mod: 'tr2-level', label: 'Custom Level' },
    ],
    '3': [
        { mod: 'tr3',       label: 'Original Game' },
        { mod: 'tr3-la',    label: 'The Lost Artifact' },
        { mod: 'tr3-level', label: 'Custom Level' },
    ],
};

function _trxShowResetDefaultsDialog() {
    var dialog = document.getElementById('reset-defaults-dialog');
    var progressDiv = document.getElementById('reset-defaults-progress');
    var progressFill = document.getElementById('reset-defaults-fill');
    var status = document.getElementById('reset-defaults-status');
    var buttons = document.getElementById('reset-defaults-buttons');
    var confirmBtn = document.getElementById('btn-reset-confirm');
    var cancelBtn = document.getElementById('btn-reset-cancel');

    progressDiv.classList.add('hidden');
    progressFill.style.width = '0%';
    buttons.classList.remove('hidden');
    dialog.classList.remove('hidden');

    function cleanup() {
        dialog.classList.add('hidden');
        confirmBtn.removeEventListener('click', onConfirm);
        cancelBtn.removeEventListener('click', onCancel);
    }

    function onConfirm() {
        confirmBtn.removeEventListener('click', onConfirm);
        cancelBtn.removeEventListener('click', onCancel);
        buttons.classList.add('hidden');
        progressDiv.classList.remove('hidden');
        status.textContent = 'Resetting...';

        // Clear SW caches so stale .js/.wasm are not served,
        // but only when online (offline reload needs the cache).
        var swCleanup = navigator.onLine
            ? caches.keys().then(function(names) {
                return Promise.all(names.map(function(n) {
                    return caches.delete(n);
                }));
            })
            : Promise.resolve();
        Promise.all([
            swCleanup,
            _trxProfileManager.resetDefaults(_trxDefaultManifest, function (done, total, name) {
                var pct = total > 0 ? Math.round((done / total) * 100) : 0;
                progressFill.style.width = pct + '%';
                if (name) {
                    status.textContent = 'Clearing ' + name + '...';
                }
            })
        ]).then(function () {
            progressFill.style.width = '100%';
            status.textContent = 'Done! Reloading...';
            setTimeout(function () { window.location.reload(); }, 500);
        });
    }

    function onCancel() {
        cleanup();
    }

    confirmBtn.addEventListener('click', onConfirm);
    cancelBtn.addEventListener('click', onCancel);
}

function _trxShowDeleteDialog(profile, refresh) {
    var dialog = document.getElementById('delete-profile-dialog');
    var text = document.getElementById('delete-profile-text');
    var spinner = document.getElementById('delete-profile-spinner');
    var buttons = document.getElementById('delete-profile-buttons');
    var confirmBtn = document.getElementById('btn-delete-confirm');
    var cancelBtn = document.getElementById('btn-delete-cancel');

    text.textContent = 'Delete profile \u201c' + profile.name + '\u201d and all its game data?';
    spinner.classList.add('hidden');
    buttons.classList.remove('hidden');
    dialog.classList.remove('hidden');

    function cleanup() {
        dialog.classList.add('hidden');
        confirmBtn.removeEventListener('click', onConfirm);
        cancelBtn.removeEventListener('click', onCancel);
    }

    function onConfirm() {
        confirmBtn.removeEventListener('click', onConfirm);
        cancelBtn.removeEventListener('click', onCancel);
        buttons.classList.add('hidden');
        spinner.classList.remove('hidden');
        _trxProfileManager.deleteProfile(profile.id).then(function () {
            cleanup();
            refresh();
        });
    }

    function onCancel() {
        cleanup();
    }

    confirmBtn.addEventListener('click', onConfirm);
    cancelBtn.addEventListener('click', onCancel);
}

function _trxShowCreateProfileDialog(onCreated) {
    var dialog = document.getElementById('create-profile-dialog');
    var nameInput = document.getElementById('new-profile-name');
    var engineSelect = document.getElementById('new-profile-engine');
    var modSelect = document.getElementById('new-profile-mod');
    var descInput = document.getElementById('new-profile-desc');

    dialog.classList.remove('hidden');
    nameInput.value = '';
    descInput.value = '';
    engineSelect.value = '';
    modSelect.innerHTML = '<option value="">Select game...</option>';
    modSelect.disabled = true;
    nameInput.focus();

    function onEngineChange() {
        var engine = engineSelect.value;
        modSelect.innerHTML = '<option value="">Select game...</option>';
        if (engine && _trxGamesByEngine[engine]) {
            var games = _trxGamesByEngine[engine];
            for (var i = 0; i < games.length; i++) {
                var opt = document.createElement('option');
                opt.value = games[i].mod;
                opt.textContent = games[i].label;
                modSelect.appendChild(opt);
            }
            modSelect.disabled = false;
        } else {
            modSelect.disabled = true;
        }
    }

    var confirmBtn = document.getElementById('btn-create-confirm');
    var cancelBtn = document.getElementById('btn-create-cancel');

    function cleanup() {
        dialog.classList.add('hidden');
        engineSelect.removeEventListener('change', onEngineChange);
        confirmBtn.removeEventListener('click', onConfirm);
        cancelBtn.removeEventListener('click', onCancel);
    }

    function onConfirm() {
        var name = nameInput.value.trim();
        if (!name) { nameInput.focus(); return; }
        if (!modSelect.value) { modSelect.focus(); return; }
        var mod = modSelect.value;
        var desc = descInput.value.trim();

        _trxProfileManager.createProfile({
            name: name,
            mod: mod,
            description: desc,
        }).then(function (profile) {
            cleanup();
            if (onCreated) onCreated(profile);
        });
    }

    function onCancel() {
        cleanup();
    }

    engineSelect.addEventListener('change', onEngineChange);
    confirmBtn.addEventListener('click', onConfirm);
    cancelBtn.addEventListener('click', onCancel);
}
