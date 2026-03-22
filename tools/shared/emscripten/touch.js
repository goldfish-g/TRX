// Touch controls for TRX WebGL builds.
//
// Virtual D-pad and action buttons for mobile/tablet play.
// Touch input feeds directly into the C touch backend via
// Module._Touch_SetState(role, pressed).
//
// Depends on: Emscripten Module (global `Module`)

'use strict';

(function() {
    var canvas = document.getElementById('canvas');
    var overlay = document.getElementById('touch-controls');
    var buttons = overlay.querySelectorAll('.tc-btn');
    var dpad = document.getElementById('tc-dpad');
    var dpadThumb = document.getElementById('tc-dpad-thumb');

    function setRole(name, pressed) {
        var role = Module.INPUT_ROLE && Module.INPUT_ROLE[name];
        if (role !== undefined) {
            Module._Touch_SetState(role, pressed ? 1 : 0);
        }
    }

    var dpadRoles = ['UP', 'RIGHT', 'DOWN', 'LEFT'];
    var dpadState = { UP: false, RIGHT: false, DOWN: false, LEFT: false };
    var dpadTouchId = null;
    var dpadThreshold = 0.38;

    function setDpadRole(name, pressed) {
        if (dpadState[name] === pressed) {
            return;
        }
        dpadState[name] = pressed;
        setRole(name, pressed);
    }

    function resetDpad() {
        dpadRoles.forEach(function(name) { setDpadRole(name, false); });
        dpad.classList.remove('tc-press-up', 'tc-press-right', 'tc-press-down', 'tc-press-left');
        dpadThumb.style.transform = 'translate(-50%, -50%)';
    }

    function updateDpadFromTouch(touch) {
        var rect = dpad.getBoundingClientRect();
        var cx = rect.left + rect.width * 0.5;
        var cy = rect.top + rect.height * 0.5;
        var dx = touch.clientX - cx;
        var dy = touch.clientY - cy;
        var radius = Math.min(rect.width, rect.height) * 0.5;
        var deadZone = radius * 0.2;
        var dist = Math.sqrt(dx * dx + dy * dy);
        var nx = 0;
        var ny = 0;

        if (dist > 0) {
            nx = dx / dist;
            ny = dy / dist;
        }

        var thumbDist = Math.min(dist, radius * 0.68);
        dpadThumb.style.transform =
            'translate(calc(-50% + ' + (nx * thumbDist).toFixed(2) + 'px), calc(-50% + '
            + (ny * thumbDist).toFixed(2) + 'px))';

        var up = false;
        var right = false;
        var down = false;
        var left = false;

        if (dist >= deadZone) {
            up = ny < -dpadThreshold;
            right = nx > dpadThreshold;
            down = ny > dpadThreshold;
            left = nx < -dpadThreshold;
        }

        setDpadRole('UP', up);
        setDpadRole('RIGHT', right);
        setDpadRole('DOWN', down);
        setDpadRole('LEFT', left);

        dpad.classList.toggle('tc-press-up', up);
        dpad.classList.toggle('tc-press-right', right);
        dpad.classList.toggle('tc-press-down', down);
        dpad.classList.toggle('tc-press-left', left);
    }

    dpad.addEventListener('touchstart', function(e) {
        e.preventDefault();
        if (dpadTouchId !== null) {
            return;
        }
        var t = e.changedTouches[0];
        dpadTouchId = t.identifier;
        updateDpadFromTouch(t);
        canvas.focus();
    }, { passive: false });

    dpad.addEventListener('touchmove', function(e) {
        e.preventDefault();
        if (dpadTouchId === null) {
            return;
        }
        for (var i = 0; i < e.changedTouches.length; i++) {
            var t = e.changedTouches[i];
            if (t.identifier === dpadTouchId) {
                updateDpadFromTouch(t);
                break;
            }
        }
    }, { passive: false });

    function maybeEndDpadTouch(e) {
        e.preventDefault();
        if (dpadTouchId === null) {
            return;
        }
        for (var i = 0; i < e.changedTouches.length; i++) {
            if (e.changedTouches[i].identifier === dpadTouchId) {
                dpadTouchId = null;
                resetDpad();
                break;
            }
        }
    }

    dpad.addEventListener('touchend', maybeEndDpadTouch, { passive: false });
    dpad.addEventListener('touchcancel', maybeEndDpadTouch, { passive: false });

    // ------------------------------------------------------------------
    // Action-button touch handling with area-based multi-press.
    // Each touch tracks ALL buttons overlapping the finger's contact
    // area (via Touch.radiusX/radiusY), so a single finger can hold
    // two adjacent buttons simultaneously.  Sliding is also supported:
    // moving the finger updates the covered set in real time.
    // ------------------------------------------------------------------

    var touchBtnMap = {};  // touchId -> [btn, btn, …]

    // Return every visible action button whose bounding circle overlaps
    // the touch contact area.
    function findBtnsForTouch(touch) {
        var cx = touch.clientX;
        var cy = touch.clientY;
        // Use the browser-reported contact radius when available, but
        // fall back to a fingertip-sized minimum (~17 CSS px) so that
        // adjacent orbit buttons can always be co-pressed.
        var touchR = Math.max(touch.radiusX || 0, touch.radiusY || 0, 17);
        var result = [];
        for (var i = 0; i < buttons.length; i++) {
            var btn = buttons[i];
            if (btn.closest('.tc-dpad')) {
                continue;
            }
            var rect = btn.getBoundingClientRect();
            if (rect.width === 0) {
                continue; // hidden (display: none)
            }
            var btnCx = rect.left + rect.width * 0.5;
            var btnCy = rect.top + rect.height * 0.5;
            var btnR = Math.min(rect.width, rect.height) * 0.5;
            var dx = cx - btnCx;
            var dy = cy - btnCy;
            if (dx * dx + dy * dy < (touchR + btnR) * (touchR + btnR)) {
                result.push(btn);
            }
        }
        return result;
    }

    function syncBtnState(btn) {
        var pressed = false;
        for (var id in touchBtnMap) {
            var btns = touchBtnMap[id];
            for (var j = 0; j < btns.length; j++) {
                if (btns[j] === btn) {
                    pressed = true;
                    break;
                }
            }
            if (pressed) {
                break;
            }
        }
        btn.classList.toggle('active', pressed);
        setRole(btn.getAttribute('data-role'), pressed);
    }

    function syncBtnList(list) {
        for (var i = 0; i < list.length; i++) {
            syncBtnState(list[i]);
        }
    }

    // Collect the union of two button arrays (no duplicates).
    function btnUnion(a, b) {
        var result = a.slice();
        for (var i = 0; i < b.length; i++) {
            if (result.indexOf(b[i]) < 0) {
                result.push(b[i]);
            }
        }
        return result;
    }

    buttons.forEach(function(btn) {
        if (btn.closest('.tc-dpad')) {
            return;
        }
        btn.addEventListener('touchstart', function(e) {
            e.preventDefault();
            for (var i = 0; i < e.changedTouches.length; i++) {
                var t = e.changedTouches[i];
                var covered = findBtnsForTouch(t);
                if (covered.indexOf(btn) < 0) {
                    covered.push(btn);
                }
                touchBtnMap[t.identifier] = covered;
                syncBtnList(covered);
            }
            canvas.focus();
        }, { passive: false });
    });

    document.addEventListener('touchmove', function(e) {
        for (var i = 0; i < e.changedTouches.length; i++) {
            var t = e.changedTouches[i];
            var oldBtns = touchBtnMap[t.identifier];
            if (!oldBtns) {
                continue;
            }
            var newBtns = findBtnsForTouch(t);
            touchBtnMap[t.identifier] = newBtns;
            syncBtnList(btnUnion(oldBtns, newBtns));
        }
    }, { passive: true });

    function onBtnTouchEnd(e) {
        for (var i = 0; i < e.changedTouches.length; i++) {
            var t = e.changedTouches[i];
            var btns = touchBtnMap[t.identifier];
            if (!btns) {
                continue;
            }
            delete touchBtnMap[t.identifier];
            syncBtnList(btns);
        }
    }

    document.addEventListener('touchend', onBtnTouchEnd);
    document.addEventListener('touchcancel', onBtnTouchEnd);

    // Expose visibility control for C (via EM_JS).
    // Also toggles the engine-3 class so TR3-only buttons appear.
    Module.setTouchControlsVisible = function(show) {
        overlay.style.display = show ? 'block' : 'none';
        if (show && typeof _trxCurrentProfile !== 'undefined' && _trxCurrentProfile) {
            var modDef = typeof MOD_DEFINITIONS !== 'undefined'
                ? MOD_DEFINITIONS[_trxCurrentProfile.mod] : null;
            var engine = modDef ? modDef.engine : _trxCurrentProfile.engine;
            overlay.classList.toggle('engine-3', engine === 3);
        }
    };
})();
