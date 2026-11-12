package com.mlbb.esp.pure;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.graphics.Outline;
import android.graphics.SurfaceTexture;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.os.Build;
import android.util.Log;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.widget.FrameLayout;

/** Looping video behind the overlay menu. Muted. Pauses when the menu closes. */
final class MenuBgPanel extends FrameLayout implements TextureView.SurfaceTextureListener {
    private static final String TAG = "MenuBgPanel";

    private final TextureView textureView;
    private MediaPlayer player;
    private Surface surface;
    private boolean playerPrepared;
    private boolean menuWantsVisible;

    MenuBgPanel(Context context) {
        super(context);
        setBackgroundColor(0);
        setClickable(false);
        setFocusable(false);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            setOutlineProvider(
                    new ViewOutlineProvider() {
                        @Override
                        public void getOutline(View view, Outline outline) {
                            outline.setRoundRect(0, 0, view.getWidth(), view.getHeight(), 18f);
                        }
                    });
            setClipToOutline(true);
        }

        textureView = new TextureView(context);
        textureView.setSurfaceTextureListener(this);
        textureView.setOpaque(false);
        addView(textureView, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        setVisibility(GONE);
        ensurePlayer(context);
    }

    private void ensurePlayer(Context context) {
        if (player != null) return;
        try {
            player = new MediaPlayer();
            player.setLooping(true);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                player.setAudioAttributes(
                        new AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_GAME)
                                .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                                .build());
            }
            player.setVolume(0f, 0f);
            try (AssetFileDescriptor afd =
                    context.getResources().openRawResourceFd(R.raw.xorel_menu_bg)) {
                player.setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
            }
            player.setOnPreparedListener(
                    mp -> {
                        playerPrepared = true;
                        if (menuWantsVisible && getVisibility() == VISIBLE) startIfReady();
                    });
            player.setOnErrorListener(
                    (mp, what, extra) -> {
                        Log.e(TAG, "MediaPlayer error what=" + what + " extra=" + extra);
                        return true;
                    });
            player.prepareAsync();
        } catch (Throwable t) {
            Log.e(TAG, "ensurePlayer", t);
        }
    }

    void onMenuVisibleChanged(boolean visible) {
        menuWantsVisible = visible;
        if (!visible) {
            setVisibility(GONE);
            pausePlayback();
            return;
        }
        setVisibility(VISIBLE);
        startIfReady();
    }

    private void startIfReady() {
        if (player == null || !playerPrepared) return;
        try {
            player.setVolume(0f, 0f);
            if (!player.isPlaying()) player.start();
        } catch (Throwable t) {
            Log.w(TAG, "start", t);
        }
    }

    private void pausePlayback() {
        try {
            if (player != null && player.isPlaying()) player.pause();
        } catch (Throwable t) {
            Log.w(TAG, "pause", t);
        }
    }

    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture st, int width, int height) {
        if (surface != null) surface.release();
        surface = new Surface(st);
        try {
            if (player != null) {
                player.setSurface(surface);
                if (menuWantsVisible && playerPrepared) startIfReady();
            }
        } catch (Throwable t) {
            Log.w(TAG, "surfaceAvailable", t);
        }
    }

    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture surfaceTexture, int width, int height) {}

    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture surfaceTexture) {
        try {
            if (player != null) player.setSurface(null);
        } catch (Throwable ignored) {
        }
        if (surface != null) {
            surface.release();
            surface = null;
        }
        return true;
    }

    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture surfaceTexture) {}

    void releaseAll() {
        pausePlayback();
        try {
            if (player != null) {
                player.release();
                player = null;
            }
        } catch (Throwable ignored) {
        }
        playerPrepared = false;
        if (surface != null) {
            surface.release();
            surface = null;
        }
    }
}
