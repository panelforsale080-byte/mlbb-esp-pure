package com.mlbb.esp.pure;

import android.content.Context;
import android.view.MotionEvent;
import android.widget.FrameLayout;

/**
 * Full-screen overlay root. When the ImGui menu is closed, do not claim any touch
 * (Android 13–16 / API 34+ OEMs may still deliver events under FLAG_NOT_TOUCHABLE).
 */
final class OverlayRootLayout extends FrameLayout {
    OverlayRootLayout(Context context) {
        super(context);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        OverlayService svc = OverlayService.instance;
        if (svc == null || !svc.isMenuInteractive()) return false;
        return super.dispatchTouchEvent(ev);
    }
}
