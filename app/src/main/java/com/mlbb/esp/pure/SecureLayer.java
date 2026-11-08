package com.mlbb.esp.pure;

import android.util.Log;
import android.view.SurfaceView;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/** Mark the GL overlay layer secure so recordings skip it while the phone still shows it. */
final class SecureLayer {
    private static final String TAG = "XorelOverlay";

    private SecureLayer() {}

    static void apply(SurfaceView view, boolean secure) {
        if (view == null) return;
        try {
            view.setSecure(secure);
        } catch (Throwable t) {
            Log.w(TAG, "setSecure", t);
        }
        boolean marked = mark(view, "mBlastSurfaceControl", secure) || mark(view, "mSurfaceControl", secure);
        try {
            Method update = SurfaceView.class.getDeclaredMethod("updateSurface");
            update.setAccessible(true);
            update.invoke(view);
            marked = true;
        } catch (Throwable t) {
            Log.w(TAG, "updateSurface", t);
        }
        Log.i(TAG, "capture protect secure=" + secure + " marked=" + marked);
    }

    private static boolean mark(SurfaceView view, String fieldName, boolean secure) {
        try {
            Field field = SurfaceView.class.getDeclaredField(fieldName);
            field.setAccessible(true);
            Object surfaceControl = field.get(view);
            if (surfaceControl == null) return false;
            Class<?> transactionClass = Class.forName("android.view.SurfaceControl$Transaction");
            Object transaction = transactionClass.getDeclaredConstructor().newInstance();
            Method setSecure = null;
            for (Method method : transactionClass.getMethods()) {
                if ("setSecure".equals(method.getName()) && method.getParameterTypes().length == 2) {
                    setSecure = method;
                    break;
                }
            }
            if (setSecure == null) return false;
            setSecure.invoke(transaction, surfaceControl, secure);
            transactionClass.getMethod("apply").invoke(transaction);
            return true;
        } catch (Throwable t) {
            Log.w(TAG, fieldName, t);
            return false;
        }
    }
}
