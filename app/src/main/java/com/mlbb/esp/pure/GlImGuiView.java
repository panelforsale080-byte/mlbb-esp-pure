package com.mlbb.esp.pure;

import android.content.Context;
import android.graphics.PixelFormat;
import android.opengl.GLSurfaceView;
import android.util.AttributeSet;
import android.util.Log;
import android.view.MotionEvent;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class GlImGuiView extends GLSurfaceView implements GLSurfaceView.Renderer {
    private static final String TAG = "GlImGuiView";
    private volatile boolean surfaceReady = false;

    public GlImGuiView(Context context) {
        super(context);
        initGl();
    }

    public GlImGuiView(Context context, AttributeSet attrs) {
        super(context, attrs);
        initGl();
    }

    public interface OnGlReadyListener {
        void onGlReady(GlImGuiView view);
    }

    private OnGlReadyListener readyListener;

    public void setOnGlReadyListener(OnGlReadyListener listener) {
        readyListener = listener;
    }

    public void configureOverlaySurface() {
        setZOrderOnTop(true);
        getHolder().setFormat(PixelFormat.TRANSLUCENT);
    }

    private void initGl() {
        setEGLContextClientVersion(2);
        setEGLConfigChooser(8, 8, 8, 8, 0, 0);
        getHolder().setFormat(PixelFormat.TRANSLUCENT);
        setPreserveEGLContextOnPause(false);
        setRenderer(this);
        setRenderMode(RENDERMODE_WHEN_DIRTY);
    }

    public void releaseGl() {
        surfaceReady = false;
        queueEvent(() -> {
            try {
                NativeBridge.nativeGlSurfaceDestroyed();
            } catch (Throwable t) {
                Log.e(TAG, "destroy", t);
            }
        });
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        try {
            NativeBridge.nativeGlSurfaceCreated();
        } catch (Throwable t) {
            Log.e(TAG, "surfaceCreated", t);
            surfaceReady = false;
        }
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        if (width <= 0 || height <= 0) return;
        try {
            NativeBridge.nativeGlSurfaceChanged(width, height);
            surfaceReady = true;
            setRenderMode(RENDERMODE_CONTINUOUSLY);
            requestRender();
            if (readyListener != null) readyListener.onGlReady(this);
        } catch (Throwable t) {
            Log.e(TAG, "surfaceChanged", t);
            surfaceReady = false;
        }
    }

    @Override
    public void onDrawFrame(GL10 gl) {
        if (!surfaceReady) return;
        try {
            NativeBridge.nativeGlDrawFrame();
        } catch (Throwable t) {
            Log.e(TAG, "drawFrame", t);
            surfaceReady = false;
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (!surfaceReady || !NativeBridge.nativeIsMenuInteractive()) return false;
        try {
            NativeBridge.nativeTouch(event.getActionMasked(), event.getX(), event.getY());
            requestRender();
        } catch (Throwable t) {
            Log.e(TAG, "touch", t);
            return false;
        }
        return true;
    }
}
