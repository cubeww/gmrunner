package dev.gmrunner;

import android.content.Intent;
import android.os.Bundle;
import android.view.View;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;

/**
 * SDL3 activity hosting the native gmrunner C++20 runtime (libSDL3.so + libmain.so).
 */
public class GmRunnerActivity extends SDLActivity {
    public static final String EXTRA_GAME_PATH = "dev.gmrunner.EXTRA_GAME_PATH";
    public static final String EXTRA_SAVE_DIR = "dev.gmrunner.EXTRA_SAVE_DIR";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        applyImmersiveMode();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveMode();
        }
    }

    private void applyImmersiveMode() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
        );
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "main"
        };
    }

    @Override
    protected String[] getArguments() {
        ArrayList<String> args = new ArrayList<>();
        Intent intent = getIntent();
        if (intent != null) {
            String saveDir = intent.getStringExtra(EXTRA_SAVE_DIR);
            if (saveDir != null && !saveDir.isEmpty()) {
                args.add("--save-dir");
                args.add(saveDir);
            }
            args.add("--touch-overlay");
            String gamePath = intent.getStringExtra(EXTRA_GAME_PATH);
            if (gamePath != null && !gamePath.isEmpty()) {
                args.add(gamePath);
            }
        }
        return args.toArray(new String[0]);
    }
}
