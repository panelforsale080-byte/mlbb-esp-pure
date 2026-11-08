package com.mlbb.esp.pure;

import android.Manifest;
import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Bundle;
import android.widget.EditText;
import android.provider.Settings;
import android.view.View;
import android.view.animation.AccelerateDecelerateInterpolator;
import android.view.animation.OvershootInterpolator;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.content.ContextCompat;

/**
 * Xorel launcher — license check, overlay permission, then floating UI.
 */
public class EspActivity extends Activity {
    private static final int REQ_NOTIF = 1001;
    private static final String MLBB_PACKAGE = "com.mobile.legends";

    private boolean pendingOverlay = false;
    private boolean licenseOk = false;

    private View contentColumn;
    private ImageView logo;
    private TextView title;
    private TextView tagline;
    private EditText licensePassword;
    private TextView keyStatus;
    private ProgressBar keyProgress;
    private SharedPreferences prefs;
    private Button btnOverlay;
    private Button btnStart;
    private Button btnGame;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_xorel);

        contentColumn = findViewById(R.id.contentColumn);
        logo = findViewById(R.id.logo);
        title = findViewById(R.id.title);
        tagline = findViewById(R.id.tagline);
        prefs = getSharedPreferences("xorel_license", MODE_PRIVATE);
        licensePassword = findViewById(R.id.licensePassword);
        keyStatus = findViewById(R.id.keyStatus);
        keyProgress = findViewById(R.id.keyProgress);
        final String savedPass = prefs.getString("password", "");
        if (!savedPass.isEmpty()) licensePassword.setText(savedPass);
        btnOverlay = findViewById(R.id.btnOverlay);
        btnStart = findViewById(R.id.btnStart);
        btnGame = findViewById(R.id.btnGame);

        btnOverlay.setOnClickListener(v -> openOverlaySettings());
        btnStart.setOnClickListener(v -> tryStartOverlay());
        btnGame.setOnClickListener(v -> openMobileLegends());

        runEntranceAnimation();
        verifyLicense();
        refreshUiState();
    }

    private void runEntranceAnimation() {
        float dy = 48f;
        contentColumn.setAlpha(0f);
        contentColumn.setTranslationY(dy);
        logo.setAlpha(0f);
        logo.setScaleX(0.82f);
        logo.setScaleY(0.82f);
        title.setAlpha(0f);
        title.setTranslationY(16f);
        tagline.setAlpha(0f);
        tagline.setTranslationY(12f);

        AnimatorSet rootIn = new AnimatorSet();
        rootIn.playTogether(
                ObjectAnimator.ofFloat(contentColumn, View.ALPHA, 0f, 1f),
                ObjectAnimator.ofFloat(contentColumn, View.TRANSLATION_Y, dy, 0f));
        rootIn.setDuration(520);
        rootIn.setInterpolator(new AccelerateDecelerateInterpolator());

        AnimatorSet hero = new AnimatorSet();
        hero.playTogether(
                ObjectAnimator.ofFloat(logo, View.ALPHA, 0f, 1f),
                ObjectAnimator.ofFloat(logo, View.SCALE_X, 0.82f, 1f),
                ObjectAnimator.ofFloat(logo, View.SCALE_Y, 0.82f, 1f));
        hero.setDuration(640);
        hero.setInterpolator(new OvershootInterpolator(0.9f));

        AnimatorSet textIn = new AnimatorSet();
        textIn.playTogether(
                ObjectAnimator.ofFloat(title, View.ALPHA, 0f, 1f),
                ObjectAnimator.ofFloat(title, View.TRANSLATION_Y, 16f, 0f),
                ObjectAnimator.ofFloat(tagline, View.ALPHA, 0f, 1f),
                ObjectAnimator.ofFloat(tagline, View.TRANSLATION_Y, 12f, 0f));
        textIn.setDuration(400);
        textIn.setStartDelay(120);

        AnimatorSet all = new AnimatorSet();
        all.playTogether(rootIn, hero);
        all.play(textIn).after(hero);
        all.start();

        /* Subtle continuous pulse on logo */
        logo.postDelayed(
                () -> {
                    if (isFinishing() || logo == null) return;
                    ObjectAnimator pulseX = ObjectAnimator.ofFloat(logo, View.SCALE_X, 1f, 1.055f);
                    pulseX.setRepeatCount(ValueAnimator.INFINITE);
                    pulseX.setRepeatMode(ValueAnimator.REVERSE);
                    pulseX.setDuration(1800);
                    pulseX.setInterpolator(new AccelerateDecelerateInterpolator());
                    ObjectAnimator pulseY = ObjectAnimator.ofFloat(logo, View.SCALE_Y, 1f, 1.055f);
                    pulseY.setRepeatCount(ValueAnimator.INFINITE);
                    pulseY.setRepeatMode(ValueAnimator.REVERSE);
                    pulseY.setDuration(1800);
                    pulseY.setInterpolator(new AccelerateDecelerateInterpolator());
                    pulseX.start();
                    pulseY.start();
                },
                900);
    }

    private void verifyLicense() {
        keyProgress.setVisibility(View.VISIBLE);
        keyStatus.setText(R.string.key_checking);
        keyStatus.setTextColor(ContextCompat.getColor(this, R.color.xorel_text_primary));
        btnStart.setEnabled(false);

        final String pass = licensePassword.getText().toString().trim();
        LicenseGate.verifyAsync(
                pass,
                (ok, message) -> {
                    licenseOk = ok;
                    if (ok) prefs.edit().putString("password", pass).apply();
                    else LicenseNative.nativeSetLicenseAttested(false);
                    keyProgress.setVisibility(View.GONE);
                    if (ok) {
                        keyStatus.setText(R.string.key_valid);
                        keyStatus.setTextColor(ContextCompat.getColor(this, R.color.xorel_success));
                    } else {
                        if ("expired".equals(message)) keyStatus.setText(R.string.key_expired);
                        else if ("password".equals(message)) keyStatus.setText(R.string.key_wrong_password);
                        else if ("signature".equals(message)) keyStatus.setText(R.string.key_bad_signature);
                        else if ("invalid".equals(message)) keyStatus.setText(R.string.key_invalid_format);
                        else keyStatus.setText(R.string.key_network_error);
                        keyStatus.setTextColor(ContextCompat.getColor(this, R.color.xorel_error));
                    }
                    refreshUiState();
                    /* Small feedback scale on license card (must match root type in layout — LinearLayout) */
                    View card = findViewById(R.id.cardLicense);
                    if (card != null) {
                        card.setScaleX(0.97f);
                        card.setScaleY(0.97f);
                        card.animate().scaleX(1f).scaleY(1f).setDuration(280).start();
                    }
                });
    }

    private void refreshUiState() {
        boolean overlayOk =
                Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this);
        boolean notifOk =
                Build.VERSION.SDK_INT < 33
                        || ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                                == PackageManager.PERMISSION_GRANTED;
        btnStart.setEnabled(licenseOk && overlayOk && notifOk);
        btnStart.setAlpha(btnStart.isEnabled() ? 1f : 0.45f);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (pendingOverlay && canOverlay()) {
            pendingOverlay = false;
            startOverlay();
        }
        refreshUiState();
    }

    private void openOverlaySettings() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            pendingOverlay = true;
            startActivity(
                    new Intent(
                            Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                            Uri.parse("package:" + getPackageName())));
        }
    }

    private void tryStartOverlay() {
        if (!licenseOk) {
            Toast.makeText(this, R.string.key_wrong_password, Toast.LENGTH_LONG).show();
            verifyLicense();
            return;
        }
        if (!canOverlay()) {
            openOverlaySettings();
            Toast.makeText(this, R.string.toast_need_overlay, Toast.LENGTH_LONG).show();
            return;
        }
        if (Build.VERSION.SDK_INT >= 33) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQ_NOTIF);
                pendingOverlay = true;
                return;
            }
        }
        startOverlay();
    }

    private void startOverlay() {
        if (!licenseOk) {
            Toast.makeText(this, R.string.key_wrong_password, Toast.LENGTH_LONG).show();
            return;
        }
        if (OverlayService.instance != null) {
            Toast.makeText(this, R.string.overlay_running, Toast.LENGTH_SHORT).show();
            moveTaskToBack(true);
            return;
        }
        try {
            Intent svc = new Intent(this, OverlayService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc);
            else startService(svc);
            Toast.makeText(this, R.string.toast_overlay_started, Toast.LENGTH_LONG).show();
            refreshUiState();
            moveTaskToBack(true);
        } catch (Throwable t) {
            Toast.makeText(this, t.getMessage() != null ? t.getMessage() : "Failed", Toast.LENGTH_LONG).show();
        }
    }

    private void openMobileLegends() {
        try {
            Intent launch = getPackageManager().getLaunchIntentForPackage(MLBB_PACKAGE);
            if (launch != null) {
                launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                startActivity(launch);
            } else {
                Toast.makeText(this, "Game not installed", Toast.LENGTH_LONG).show();
            }
        } catch (Throwable t) {
            Toast.makeText(this, "Cannot open game", Toast.LENGTH_SHORT).show();
        }
    }

    private boolean canOverlay() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this);
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        if (code == REQ_NOTIF && pendingOverlay) tryStartOverlay();
        refreshUiState();
    }
}
