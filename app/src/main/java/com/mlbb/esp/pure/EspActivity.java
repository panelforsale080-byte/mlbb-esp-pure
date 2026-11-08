package com.mlbb.esp.pure;

import android.Manifest;
import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Bundle;
import android.widget.EditText;
import android.provider.Settings;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.graphics.Typeface;
import android.view.View;
import android.view.ViewGroup;
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
    private Button btnCheckLicense;
    private Button btnOverlay;
    private Button btnStart;
    private Button btnGame;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_xorel);

        contentColumn = findViewById(R.id.contentColumn);
        applyLauncherFont(contentColumn);
        logo = findViewById(R.id.logo);
        title = findViewById(R.id.title);
        tagline = findViewById(R.id.tagline);
        prefs = getSharedPreferences("xorel_license", MODE_PRIVATE);
        licensePassword = findViewById(R.id.licensePassword);
        final String savedPass = prefs.getString("password", "");
        if (!savedPass.isEmpty()) licensePassword.setText(savedPass);
        keyStatus = findViewById(R.id.keyStatus);
        keyProgress = findViewById(R.id.keyProgress);
        btnCheckLicense = findViewById(R.id.btnCheckLicense);
        btnOverlay = findViewById(R.id.btnOverlay);
        btnStart = findViewById(R.id.btnStart);
        btnGame = findViewById(R.id.btnGame);

        btnCheckLicense.setOnClickListener(v -> verifyLicense());
        licensePassword.setImeOptions(EditorInfo.IME_ACTION_DONE);
        licensePassword.setOnEditorActionListener((v, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_DONE || actionId == EditorInfo.IME_ACTION_GO) {
                verifyLicense();
                return true;
            }
            return false;
        });
        btnOverlay.setOnClickListener(v -> openOverlaySettings());
        btnStart.setOnClickListener(v -> tryStartOverlay());
        btnGame.setOnClickListener(v -> openMobileLegends());

        keyProgress.setVisibility(View.GONE);
        keyStatus.setVisibility(View.GONE);
        keyStatus.setText("");
        try {
            LicenseNative.nativeSetLicenseAttested(false);
        } catch (Throwable ignored) {
        }

        runEntranceAnimation();
        refreshUiState();
    }

    /** Asset font, not res/font — clone apps still inflate the layout if the TTF is missing. */
    private void applyLauncherFont(View root) {
        if (root == null) return;
        Typeface face;
        try {
            face = Typeface.createFromAsset(getAssets(), "monocraft.ttf");
        } catch (Throwable ignored) {
            return;
        }
        if (face == null) return;
        applyTypeface(root, face);
    }

    private void applyTypeface(View view, Typeface face) {
        if (view instanceof TextView) ((TextView) view).setTypeface(face);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) applyTypeface(group.getChildAt(i), face);
        }
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
    }

    private void verifyLicense() {
        final String pass = licensePassword.getText().toString().trim();
        if (pass.isEmpty()) {
            licenseOk = false;
            LicenseNative.nativeSetLicenseAttested(false);
            showLicenseSpinner(false);
            keyStatus.setVisibility(View.GONE);
            refreshUiState();
            return;
        }

        licenseOk = false;
        InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        if (imm != null) imm.hideSoftInputFromWindow(licensePassword.getWindowToken(), 0);
        showLicenseSpinner(true);
        keyStatus.setVisibility(View.GONE);
        btnStart.setEnabled(false);

        LicenseGate.verifyAsync(
                pass,
                (ok, message) -> {
                    if (isFinishing()) return;
                    showLicenseSpinner(false);
                    licenseOk = ok;
                    if (ok) prefs.edit().putString("password", pass).apply();
                    else LicenseNative.nativeSetLicenseAttested(false);
                    keyStatus.setVisibility(View.VISIBLE);
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
                });
    }

    private void showLicenseSpinner(boolean on) {
        keyProgress.setVisibility(on ? View.VISIBLE : View.GONE);
        btnCheckLicense.setEnabled(!on);
        btnCheckLicense.setText(on ? "" : getString(R.string.btn_check_license));
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
            Toast.makeText(this, R.string.key_enter_password, Toast.LENGTH_LONG).show();
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
            Toast.makeText(this, R.string.key_enter_password, Toast.LENGTH_LONG).show();
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
