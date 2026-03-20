// File upload screen for TRX WebGL builds.
//
// Handles drag-and-drop / file-picker uploads, archive extraction,
// language selection, FMV warnings, and progress display.  After
// processing, writes files to the VFS, generates gameflow for custom
// levels, persists to IndexedDB, and launches the game.
//
// Depends on: Emscripten FS             (global `FS`)
//             GameDataManager           (from gamedata.js)
//             _trxSetupCustomGameflow   (from gameflow.js)
//             _trxProfileManager        (global from shell.html)
//             _trxStartGame, _trxResumeWithProfile,
//             _trxShowProfileSelector   (from ui.js)

'use strict';

function _trxShowUploadUI(profile, callback, refresh) {
    var screen = document.getElementById('upload-screen');
    var dropzone = document.getElementById('upload-dropzone');
    var progressDiv = document.getElementById('upload-progress');
    var progressFill = document.getElementById('upload-progress-fill');
    var progressText = document.getElementById('upload-progress-text');
    var errorEl = document.getElementById('upload-error');
    var folderInput = document.getElementById('file-input-folder');
    var archiveInput = document.getElementById('file-input-archive');
    var backBtn = document.getElementById('btn-upload-back');
    var subtitle = document.getElementById('upload-subtitle');

    document.getElementById('profile-selector').classList.add('hidden');
    screen.classList.remove('hidden');
    subtitle.textContent = 'Upload game data for: ' + profile.name;
    backBtn.classList.remove('hidden');

    // Reset state
    progressDiv.classList.add('hidden');
    errorEl.classList.add('hidden');

    var gdm = new GameDataManager(profile.modDir || profile.mod);

    function showError(msg) {
        errorEl.textContent = msg;
        errorEl.classList.remove('hidden');
    }

    function hideError() {
        errorEl.classList.add('hidden');
    }

    var warningDiv = document.getElementById('upload-warning');
    var warningText = document.getElementById('upload-warning-text');

    function showWarning(msg) {
        return new Promise(function(resolve, reject) {
            progressDiv.classList.add('hidden');
            warningText.textContent = msg;
            warningDiv.classList.remove('hidden');

            var btnContinue = document.getElementById('btn-warning-continue');
            var btnCancel = document.getElementById('btn-warning-cancel');

            function cleanup() {
                btnContinue.removeEventListener('click', onContinue);
                btnCancel.removeEventListener('click', onCancel);
                warningDiv.classList.add('hidden');
            }
            function onContinue() {
                cleanup();
                progressDiv.classList.remove('hidden');
                resolve();
            }
            function onCancel() {
                cleanup();
                reject(new Error('Upload cancelled.'));
            }

            btnContinue.addEventListener('click', onContinue);
            btnCancel.addEventListener('click', onCancel);
        });
    }

    var conversionDiv = document.getElementById('upload-conversion-warning');
    var outfitCheckbox = document.getElementById('upload-outfit-checkbox');

    function showConversionWarning() {
        return new Promise(function(resolve, reject) {
            progressDiv.classList.add('hidden');
            outfitCheckbox.checked = false;
            conversionDiv.classList.remove('hidden');

            var btnContinue = document.getElementById('btn-conversion-continue');
            var btnCancel = document.getElementById('btn-conversion-cancel');

            function cleanup() {
                btnContinue.removeEventListener('click', onContinue);
                btnCancel.removeEventListener('click', onCancel);
                conversionDiv.classList.add('hidden');
            }
            function onContinue() {
                var useOutfit = outfitCheckbox.checked;
                cleanup();
                progressDiv.classList.remove('hidden');
                resolve(useOutfit);
            }
            function onCancel() {
                cleanup();
                reject(new Error('Upload cancelled.'));
            }

            btnContinue.addEventListener('click', onContinue);
            btnCancel.addEventListener('click', onCancel);
        });
    }

    var langSelectDiv = document.getElementById('upload-lang-select');
    var langList = document.getElementById('upload-lang-list');

    function showLanguageSelect(languages) {
        return new Promise(function(resolve) {
            progressDiv.classList.add('hidden');
            langList.innerHTML = '';
            for (var i = 0; i < languages.length; i++) {
                (function(lang) {
                    var btn = document.createElement('button');
                    btn.type = 'button';
                    btn.textContent = lang.name;
                    btn.addEventListener('click', function() {
                        langSelectDiv.classList.add('hidden');
                        progressDiv.classList.remove('hidden');
                        resolve(lang.code);
                    });
                    langList.appendChild(btn);
                })(languages[i]);
            }
            langSelectDiv.classList.remove('hidden');
        });
    }

    function handleFiles(files) {
        if (!files || files.length === 0) return;
        hideError();
        warningDiv.classList.add('hidden');
        conversionDiv.classList.add('hidden');
        langSelectDiv.classList.add('hidden');
        progressDiv.classList.remove('hidden');
        progressFill.style.width = '0%';
        progressText.textContent = 'Processing...';

        // Custom levels almost never include FMVs — skip the warning.
        var fmvWarning = profile.modDir ? null : showWarning;

        gdm.processUpload(files, function(msg, frac) {
            progressFill.style.width = Math.round(frac * 100) + '%';
            progressText.textContent = msg;
        }, fmvWarning, showLanguageSelect).then(function(mappedFiles) {
            // Load into FS first (template gameflow must be readable)
            progressText.textContent = 'Loading into game...';
            progressFill.style.width = '80%';
            gdm.loadMappedToFS(mappedFiles);

            // For custom level profiles without a TRX-native gameflow,
            // show a conversion warning with outfit preference.
            var conversionStep = Promise.resolve(false);
            if (profile.modDir) {
                var modPrefix = 'games/' + profile.modDir + '/';
                var hasNativeGameflow = false;
                for (var i = 0; i < mappedFiles.length; i++) {
                    if (mappedFiles[i].path === modPrefix + 'gameflow.json5') {
                        hasNativeGameflow = true;
                        break;
                    }
                }
                if (!hasNativeGameflow) {
                    conversionStep = showConversionWarning();
                }
            }

            return conversionStep.then(function(useOutfitImport) {
                // Generate gameflow + strings for custom level profiles
                if (profile.modDir) {
                    _trxSetupCustomGameflow(
                        mappedFiles, profile.modDir,
                        profile.mod, useOutfitImport);
                }

                // Store to IDB (mappedFiles now includes gameflow + strings)
                progressText.textContent = 'Saving to browser storage...';
                return _trxProfileManager.storeGameData(profile.id, mappedFiles, function (stored, total) {
                    var pct = 85 + Math.round((stored / total) * 10);
                    progressFill.style.width = pct + '%';
                }).then(function () {
                    progressText.textContent = 'Done!';
                    progressFill.style.width = '100%';

                    // Start the game
                    screen.classList.add('hidden');
                    if (callback) {
                        _trxResumeWithProfile(profile);
                    } else {
                        _trxStartGame(profile);
                    }
                });
            });
        }).catch(function(err) {
            progressDiv.classList.add('hidden');
            if (err.message === 'Upload cancelled.') return;
            showError(err.message || 'Failed to process game files.');
            console.error('[TRX] Upload error:', err);
        });
    }

    // Drag and drop
    dropzone.addEventListener('dragover', function(e) {
        e.preventDefault();
        e.stopPropagation();
        dropzone.classList.add('drag-over');
    });
    dropzone.addEventListener('dragleave', function(e) {
        e.preventDefault();
        e.stopPropagation();
        dropzone.classList.remove('drag-over');
    });
    dropzone.addEventListener('drop', function(e) {
        e.preventDefault();
        e.stopPropagation();
        dropzone.classList.remove('drag-over');
        handleFiles(e.dataTransfer.files);
    });
    dropzone.addEventListener('click', function() {
        archiveInput.click();
    });

    // Button handlers
    document.getElementById('btn-select-folder').addEventListener('click', function(e) {
        e.stopPropagation();
        folderInput.click();
    });
    document.getElementById('btn-select-archive').addEventListener('click', function(e) {
        e.stopPropagation();
        archiveInput.click();
    });

    folderInput.addEventListener('change', function() {
        handleFiles(folderInput.files);
    });
    archiveInput.addEventListener('change', function() {
        handleFiles(archiveInput.files);
    });

    // Back button
    var newBackBtn = backBtn.cloneNode(true);
    backBtn.parentNode.replaceChild(newBackBtn, backBtn);
    newBackBtn.classList.remove('hidden');
    newBackBtn.addEventListener('click', function () {
        screen.classList.add('hidden');
        _trxShowProfileSelector(callback);
    });
}
