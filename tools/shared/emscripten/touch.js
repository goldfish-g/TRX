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

    buttons.forEach(function(btn) {
        if (btn.closest('.tc-dpad')) {
            return;
        }
        var role = btn.getAttribute('data-role');
        var activeTouches = {};  // touchId -> true

        btn.addEventListener('touchstart', function(e) {
            e.preventDefault();
            var dominated = Object.keys(activeTouches).length === 0;
            for (var i = 0; i < e.changedTouches.length; i++) {
                activeTouches[e.changedTouches[i].identifier] = true;
            }
            if (dominated) {
                btn.classList.add('active');
                setRole(role, true);
            }
            canvas.focus();
        }, { passive: false });

        btn.addEventListener('touchend', function(e) {
            e.preventDefault();
            for (var i = 0; i < e.changedTouches.length; i++) {
                delete activeTouches[e.changedTouches[i].identifier];
            }
            if (Object.keys(activeTouches).length === 0) {
                btn.classList.remove('active');
                setRole(role, false);
            }
        }, { passive: false });

        btn.addEventListener('touchcancel', function(e) {
            e.preventDefault();
            for (var i = 0; i < e.changedTouches.length; i++) {
                delete activeTouches[e.changedTouches[i].identifier];
            }
            if (Object.keys(activeTouches).length === 0) {
                btn.classList.remove('active');
                setRole(role, false);
            }
        }, { passive: false });
    });

    // Expose visibility control for C (via EM_JS)
    Module.setTouchControlsVisible = function(show) {
        overlay.style.display = show ? 'block' : 'none';
    };
})();
