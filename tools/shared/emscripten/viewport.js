// Viewport / fold detection for TRX WebGL builds.
//
// Keeps fold posture state in sync without forcing canvas buffer size.
// Supports dual-screen / foldable devices (Samsung Galaxy Fold, etc.).
//
// Depends on: Emscripten Module (global `Module`)

'use strict';

(function() {
    var resizeTimer = null;
    var ignoreNextResize = false;

    function signalRuntimeResize() {
        // Let SDL/Emscripten react to CSS-driven canvas size changes.
        ignoreNextResize = true;
        window.dispatchEvent(new Event('resize'));
        setTimeout(function() { ignoreNextResize = false; }, 0);
    }

    function detectHorizontalFoldSegments() {
        if (typeof window.getWindowSegments === 'function') {
            var segments = window.getWindowSegments();
            if (segments && segments.length >= 2) {
                var sorted = segments.slice().sort(function(a, b) { return a.top - b.top; });
                var topSeg = sorted[0];
                var bottomSeg = sorted[1];
                if (Math.abs(topSeg.left - bottomSeg.left) < 2
                    && Math.abs(topSeg.width - bottomSeg.width) < 2
                    && bottomSeg.top >= topSeg.bottom - 2) {
                    return { topHeight: Math.round(topSeg.height), bottomHeight: Math.round(bottomSeg.height) };
                }
            }
        }

        var isSegmented = window.matchMedia('(horizontal-viewport-segments: 1) and (vertical-viewport-segments: 2)').matches;
        var isFoldSpanning = window.matchMedia('(spanning: single-fold-horizontal)').matches;
        var isLaptopPosture = window.matchMedia('(screen-fold-posture: laptop)').matches;
        if (isSegmented || isFoldSpanning || isLaptopPosture) {
            var vh = Math.max(
                window.innerHeight || 0,
                (window.visualViewport && Math.round(window.visualViewport.height)) || 0,
                document.documentElement.clientHeight || 0);
            var topHalf = Math.round(vh * 0.5);
            return { topHeight: topHalf, bottomHeight: Math.max(0, vh - topHalf) };
        }

        return null;
    }

    function syncViewportLayout() {
        var body = document.body;
        var layout = detectHorizontalFoldSegments();
        var viewportWidth = Math.max(
            window.innerWidth || 0,
            (window.visualViewport && Math.round(window.visualViewport.width)) || 0,
            document.documentElement.clientWidth || 0);
        var viewportHeight = Math.max(
            window.innerHeight || 0,
            (window.visualViewport && Math.round(window.visualViewport.height)) || 0,
            document.documentElement.clientHeight || 0);

        if (viewportWidth < 100 || viewportHeight < 100) {
            setTimeout(syncViewportLayout, 120);
            return;
        }

        if (layout && (layout.topHeight < 60 || layout.bottomHeight < 60)) {
            layout.topHeight = Math.round(viewportHeight * 0.5);
            layout.bottomHeight = Math.max(0, viewportHeight - layout.topHeight);
        }

        if (layout) {
            body.classList.add('is-folded-horizontal');
            body.style.setProperty('--fold-top-height', layout.topHeight + 'px');
            body.style.setProperty('--fold-controls-height', layout.bottomHeight + 'px');
        } else {
            body.classList.remove('is-folded-horizontal');
            body.style.removeProperty('--fold-top-height');
            body.style.removeProperty('--fold-controls-height');
        }

        signalRuntimeResize();
    }

    function scheduleSync() {
        if (resizeTimer !== null) {
            clearTimeout(resizeTimer);
        }
        syncViewportLayout();
        resizeTimer = setTimeout(syncViewportLayout, 120);
        setTimeout(syncViewportLayout, 260);
        setTimeout(syncViewportLayout, 520);
    }

    window.addEventListener('resize', function() {
        if (ignoreNextResize) {
            return;
        }
        scheduleSync();
    });
    window.addEventListener('orientationchange', scheduleSync);
    if (window.visualViewport) {
        window.visualViewport.addEventListener('resize', scheduleSync);
    }

    [
        '(screen-fold-posture: laptop)',
        '(spanning: single-fold-horizontal)',
        '(horizontal-viewport-segments: 1) and (vertical-viewport-segments: 2)'
    ].forEach(function(q) {
        var mq = window.matchMedia(q);
        if (mq && typeof mq.addEventListener === 'function') {
            mq.addEventListener('change', scheduleSync);
        } else if (mq && typeof mq.addListener === 'function') {
            mq.addListener(scheduleSync);
        }
    });

    document.addEventListener('DOMContentLoaded', scheduleSync);
    window.addEventListener('load', scheduleSync);

    // Expose for any runtime-triggered layout refresh.
    Module.syncViewportLayout = scheduleSync;
})();
