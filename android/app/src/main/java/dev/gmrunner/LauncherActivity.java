package dev.gmrunner;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Lightweight launcher activity for Android:
 * - Lists game directories, .zip archives, and .exe files placed in app-external "games/" folder
 * - Supports opening any .zip or .exe via Android Storage Access Framework (SAF)
 * - Auto-launches a bundled APK game if placed under assets/game/
 */
public class LauncherActivity extends Activity {
    private static final int REQ_OPEN_DOCUMENT = 1001;

    private final List<File> discoveredGames = new ArrayList<>();
    private ArrayAdapter<String> listAdapter;
    private TextView statusText;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        File bundled = tryExtractBundledAssetGame();
        if (bundled != null) {
            launchGame(bundled);
            finish();
            return;
        }

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.parseColor("#0D1117"));
        root.setPadding(36, 28, 36, 28);

        TextView title = new TextView(this);
        title.setText("gmrunner — GameMaker 8.0 / 8.1 / 8.2");
        title.setTextColor(Color.WHITE);
        title.setTextSize(20f);
        root.addView(title);

        File gamesDir = getGamesDirectory();
        statusText = new TextView(this);
        statusText.setText("Place game folders or .zip archives in:\n" + gamesDir.getAbsolutePath());
        statusText.setTextColor(Color.parseColor("#8B949E"));
        statusText.setTextSize(12.5f);
        statusText.setPadding(0, 8, 0, 16);
        root.addView(statusText);

        LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        btnRow.setGravity(Gravity.START);

        Button pickBtn = new Button(this);
        pickBtn.setText("Import .zip or .exe");
        pickBtn.setOnClickListener(v -> openDocumentPicker());
        btnRow.addView(pickBtn);

        Button refreshBtn = new Button(this);
        refreshBtn.setText("Refresh List");
        refreshBtn.setOnClickListener(v -> refreshGameList());
        btnRow.addView(refreshBtn);

        root.addView(btnRow);

        ListView listView = new ListView(this);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.0f);
        lp.topMargin = 16;
        listView.setLayoutParams(lp);
        listAdapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, new ArrayList<>());
        listView.setAdapter(listAdapter);
        listView.setOnItemClickListener((parent, view, position, id) -> {
            if (position >= 0 && position < discoveredGames.size()) {
                launchGame(discoveredGames.get(position));
            }
        });
        root.addView(listView);

        setContentView(root);
        refreshGameList();
    }

    private File getGamesDirectory() {
        File base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        File games = new File(base, "games");
        if (!games.exists()) games.mkdirs();
        return games;
    }

    private File getSavesDirectory(String gameKey) {
        String clean = gameKey.replaceAll("[^a-zA-Z0-9_-]+", "_");
        if (clean.isEmpty()) clean = "default";
        File saves = new File(new File(getFilesDir(), "saves"), clean);
        if (!saves.exists()) saves.mkdirs();
        return saves;
    }

    private void refreshGameList() {
        discoveredGames.clear();
        listAdapter.clear();

        File gamesDir = getGamesDirectory();
        File[] children = gamesDir.listFiles();
        if (children != null) {
            Arrays.sort(children, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
            for (File f : children) {
                String lower = f.getName().toLowerCase();
                if (f.isDirectory() || lower.endsWith(".zip") || lower.endsWith(".exe")) {
                    discoveredGames.add(f);
                    String label = f.isDirectory() ? ("[DIR]  " + f.getName()) : f.getName();
                    listAdapter.add(label);
                }
            }
        }
        listAdapter.notifyDataSetChanged();
    }

    private void openDocumentPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQ_OPEN_DOCUMENT);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_OPEN_DOCUMENT && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) {
                importAndLaunchUri(uri);
            }
        }
    }

    private String queryDisplayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) {
                    String name = cursor.getString(idx);
                    if (name != null && !name.isEmpty()) return name;
                }
            }
        } catch (Exception ignored) {}
        return "imported_game.zip";
    }

    private void importAndLaunchUri(Uri uri) {
        String name = queryDisplayName(uri);
        File dst = new File(getGamesDirectory(), name);
        statusText.setText("Importing " + name + "...");
        new Thread(() -> {
            try (InputStream in = getContentResolver().openInputStream(uri);
                 FileOutputStream out = new FileOutputStream(dst)) {
                if (in == null) throw new IllegalStateException("Cannot open input stream");
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
                runOnUiThread(() -> {
                    refreshGameList();
                    launchGame(dst);
                });
            } catch (Exception ex) {
                runOnUiThread(() ->
                    Toast.makeText(this, "Import failed: " + ex.getMessage(), Toast.LENGTH_LONG).show()
                );
            }
        }).start();
    }

    private File tryExtractBundledAssetGame() {
        try {
            String[] entries = getAssets().list("game");
            if (entries == null || entries.length == 0) return null;
            File outDir = new File(getFilesDir(), "bundled_game");
            if (!outDir.exists()) outDir.mkdirs();
            copyAssetDir("game", outDir);
            for (String e : entries) {
                if (e.toLowerCase().endsWith(".zip") || e.toLowerCase().endsWith(".exe")) {
                    return new File(outDir, e);
                }
            }
            return outDir;
        } catch (Exception ignored) {
            return null;
        }
    }

    private void copyAssetDir(String assetPath, File dstDir) throws Exception {
        String[] entries = getAssets().list(assetPath);
        if (entries == null || entries.length == 0) {
            if (dstDir.exists() && dstDir.length() > 0) return;
            try (InputStream in = getAssets().open(assetPath);
                 FileOutputStream out = new FileOutputStream(dstDir)) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            return;
        }
        if (!dstDir.exists()) dstDir.mkdirs();
        for (String name : entries) {
            copyAssetDir(assetPath + "/" + name, new File(dstDir, name));
        }
    }

    private void launchGame(File target) {
        String baseName = target.getName();
        int dot = baseName.lastIndexOf('.');
        if (dot > 0) baseName = baseName.substring(0, dot);
        File saveDir = getSavesDirectory(baseName);

        Intent intent = new Intent(this, GmRunnerActivity.class);
        intent.putExtra(GmRunnerActivity.EXTRA_GAME_PATH, target.getAbsolutePath());
        intent.putExtra(GmRunnerActivity.EXTRA_SAVE_DIR, saveDir.getAbsolutePath());
        startActivity(intent);
    }
}
