package com.mlbb.esp.pure;

import android.content.Context;
import android.graphics.Outline;
import android.graphics.SurfaceTexture;
import android.media.MediaPlayer;
import android.net.Uri;
import android.util.AttributeSet;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.widget.FrameLayout;
import android.widget.ImageButton;

/** In-menu promo video: loop, pause on minimize, resume on reopen (not one-shot). */
final class MenuVideoPanel extends FrameLayout implements TextureView.SurfaceTextureListener {
    private static final String TAG = "MenuVideoPanel";

    private TextureView textureView;
    private ImageButton muteBtn;
    private MediaPlayer player;
    private Surface surface;
    private boolean playerPrepared;
    private boolean menuWantsVisible;
    private boolean muted;

    MenuVideoPanel(Context context) {
        super(context);
        init(context);
    }

    MenuVideoPanel(Context context, AttributeSet attrs) {
        super(context, attrs);
        init(context);
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private void init(Context context) {
        setBackgroundResource(R.drawable.bg_menu_video_frame);
        setPadding(dp(4), dp(4), dp(4), dp(4));
        setClickable(false);
        setFocusable(false);

        textureView = new TextureView(context);
        textureView.setSurfaceTextureListener(this);
        textureView.setOpaque(false);
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.LOLLIPOP) {
            final float r = dp(10);
            textureView.setOutlineProvider(
                    new ViewOutlineProvider() {
                        @Override
                        public void getOutline(View view, Outline outline) {
                            outline.setRoundRect(0, 0, view.getWidth(), view.getHeight(), r);
                        }
                    });
            textureView.setClipToOutline(true);
        }
        addView(
                textureView,
                new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        muteBtn = new ImageButton(context);
        muteBtn.setBackgroundResource(R.drawable.bg_mute_icon_btn);
        muteBtn.setScaleType(android.widget.ImageView.ScaleType.CENTER_INSIDE);
        muteBtn.setPadding(dp(6), dp(6), dp(6), dp(6));
        muteBtn.setContentDescription("Toggle sound");
        muteBtn.setOnClickListener(v -> setMuted(!muted, true));
        final int btn = dp(34);
        FrameLayout.LayoutParams muteLp =
                new LayoutParams(btn, btn, Gravity.TOP | Gravity.END);
        muteLp.topMargin = dp(2);
        muteLp.rightMargin = dp(2);
        addView(muteBtn, muteLp);

        setVisibility(GONE);
        updateMuteIcon();
        ensurePlayer(context);
    }

    private void ensurePlayer(Context context) {
        if (player != null) return;
        try {
            player = new MediaPlayer();
            player.setLooping(true);
            player.setVolume(1f, 1f);
            Uri uri =
                    Uri.parse(
                            "android.resource://"
                                    + context.getPackageName()
                                    + "/"
                                    + R.raw.xorel_menu_promo);
            player.setDataSource(context, uri);
            player.setOnPreparedListener(
                    mp -> {
                        playerPrepared = true;
                        applyVolume();
                        if (menuWantsVisible && getVisibility() == VISIBLE) {
                            try {
                                if (!mp.isPlaying()) mp.start();
                            } catch (Throwable t) {
                                Log.w(TAG, "start", t);
                            }
                        }
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
        resumePlayback();
    }

    private void pausePlayback() {
        try {
            if (player != null && player.isPlaying()) player.pause();
        } catch (Throwable t) {
            Log.w(TAG, "pause", t);
        }
    }

    private void resumePlayback() {
        if (player == null) return;
        try {
            if (playerPrepared) {
                applyVolume();
                if (!player.isPlaying()) player.start();
            }
        } catch (Throwable t) {
            Log.w(TAG, "resume", t);
        }
    }

    void setMuted(boolean value, boolean fromUser) {
        if (muted == value) return;
        muted = value;
        applyVolume();
        updateMuteIcon();
        if (fromUser) {
            try {
                NativeBridge.nativeSetMenuVideoMuted(muted);
            } catch (Throwable ignored) {
            }
        }
    }

    boolean isMuted() {
        return muted;
    }

    private void applyVolume() {
        if (player == null) return;
        try {
            final float v = muted ? 0f : 1f;
            player.setVolume(v, v);
        } catch (Throwable t) {
            Log.w(TAG, "volume", t);
        }
    }

    private void updateMuteIcon() {
        muteBtn.setImageResource(muted ? R.drawable.ic_menu_volume_off : R.drawable.ic_menu_volume_on);
    }

    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture st, int width, int height) {
        if (surface != null) {
            surface.release();
        }
        surface = new Surface(st);
        try {
            if (player != null) {
                player.setSurface(surface);
                if (menuWantsVisible && playerPrepared && !player.isPlaying()) player.start();
            }
        } catch (Throwable t) {
            Log.w(TAG, "surfaceAvailable", t);
        }
    }

    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height) {}

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
    public void onSurfaceTextureUpdated(SurfaceTexture surface) {}

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
