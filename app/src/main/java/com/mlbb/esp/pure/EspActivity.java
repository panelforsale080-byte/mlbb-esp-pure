package com.mlbb.esp.pure;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Launcher only — ImGui + ESP run in {@link OverlayService} so touches pass to MLBB when menu is closed.
 */
public class EspActivity extends Activity {
    private static final int REQ_NOTIF = 1001;
    private boolean pendingOverlay = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout shell = new LinearLayout(this);
        shell.setOrientation(LinearLayout.VERTICAL);
        shell.setBackgroundColor(0xFF121212);
        shell.setPadding(32, 48, 32, 32);

        TextView info = new TextView(this);
        info.setTextColor(0xFFEEEEEE);
        info.setText(
                "1) Allow display over other apps\n"
                        + "2) Start overlay — open MLBB\n"
                        + "3) Tap ≡ for menu (game touch when menu closed)\n"
                        + "4) Find pid → Resolve in match");
        shell.addView(info);

        Button perm = new Button(this);
        perm.setText("Allow display over other apps");
        perm.setOnClickListener(v -> openOverlaySettings());
        shell.addView(perm);

        Button start = new Button(this);
        start.setText("Start overlay");
        start.setOnClickListener(v -> tryStartOverlay());
        shell.addView(start);

        setContentView(shell);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (pendingOverlay && canOverlay()) {
            pendingOverlay = false;
            startOverlay();
        }
    }

    private void openOverlaySettings() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            pendingOverlay = true;
            startActivity(new Intent(
                    Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        }
    }

    private void tryStartOverlay() {
        if (!canOverlay()) {
            openOverlaySettings();
            Toast.makeText(this, "Allow overlay permission first", Toast.LENGTH_LONG).show();
            return;
        }
        if (Build.VERSION.SDK_INT >= 33) {
            if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQ_NOTIF);
                pendingOverlay = true;
                return;
            }
        }
        startOverlay();
    }

    private void startOverlay() {
        if (OverlayService.instance != null) {
            Toast.makeText(this, "Overlay already running", Toast.LENGTH_SHORT).show();
            moveTaskToBack(true);
            return;
        }
        try {
            Intent svc = new Intent(this, OverlayService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc);
            else startService(svc);
            Toast.makeText(this, "Overlay started — open MLBB", Toast.LENGTH_LONG).show();
            moveTaskToBack(true);
        } catch (Throwable t) {
            Toast.makeText(this, "Failed: " + t.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private boolean canOverlay() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this);
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        if (code == REQ_NOTIF && pendingOverlay) tryStartOverlay();
    }
}
