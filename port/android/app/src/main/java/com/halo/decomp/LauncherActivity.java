package com.halo.decomp;

import android.Manifest;
import android.app.Activity;
import android.app.Dialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.drawable.ColorDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.InputType;
import android.text.TextUtils;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Starts the game once its data is in place.
 *
 * The game reads the Xbox game data (the folder holding maps/) from the
 * app's external files directory, /sdcard/Android/data/com.halo.decomp/files.
 * If it is missing, this screen finds or accepts an Xbox disc image and
 * extracts maps/ out of it on the device, or copies a maps/ folder the
 * player picks.
 *
 * The screen is an ONI archive terminal: a Halo ring in the dark, holo-cyan
 * readouts and a data-recovery progress bar.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_FOLDER = 1;
    private static final int PICK_IMAGE = 2;
    private static final int REQUEST_STORAGE = 3;

    private static final int COLOR_ACTIVE = HaloUi.CYAN;
    private static final String TAG = "halo-import";

    private File dataRoot;
    private TextView status;
    private TextView readout;
    private TextView percent;
    private TextView seal;
    private HaloProgressView progress;
    private LinearLayout buttons;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final List<String> lines = new ArrayList<>();
    private final List<Candidate> haloImages = new ArrayList<>();
    private final List<Candidate> otherImages = new ArrayList<>();
    private File currentImage;
    private Uri currentUri;
    private boolean showingChooser;
    private boolean scannedOnce;
    private boolean rescanOnResume;
    private boolean archiveAccess;
    private volatile boolean extracting;
    private volatile boolean scanning;
    private volatile boolean cancelled;
    private boolean autoExtractPending = true;
    private int lastLoggedPercent = -1;

    private static final class Entry {
        final Uri uri;
        final String path;
        final long size;

        Entry(Uri uri, String path, long size) {
            this.uri = uri;
            this.path = path;
            this.size = size;
        }
    }

    /** a disc image the search found, with what probing it learned */
    private static final class Candidate {
        final File file;
        final XisoExtractor.Info info;

        Candidate(File file, XisoExtractor.Info info) {
            this.file = file;
            this.info = info;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dataRoot = getExternalFilesDir(null);
        // created by the app, so that files pushed into it with adb stay
        // readable (a directory adb creates there belongs to the shell user)
        if (dataRoot != null)
            new File(dataRoot, "maps").mkdirs();
        passOnHardwareId();
        passOnInvite(getIntent());
        buildInterface();
        Uri target = imageFromIntent(getIntent());
        if (target != null) {
            log("> archive image handed to the terminal");
            beginExtract(target, displayName(target));
            return;
        }
        if (haveData()) {
            // the terminal is the app's home screen: with more than one Halo
            // disc to choose from it opens here, so the player can switch
            // discs or just play; with only one there is nothing to ask
            List<Candidate> known = knownHalos();
            if (known.size() >= 2) {
                showingChooser = true;
                autoExtractPending = false;
                presentScan(known, new ArrayList<>(), hasArchiveAccess());
            } else {
                startGame();
            }
            return;
        }
        scanForImages();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (extracting || buttons == null)
            return;
        if (rescanOnResume) {
            rescanOnResume = false;
            scannedOnce = false;
        }
        if (haveData()) {
            if (!showingChooser)
                startGame();
            return;
        }
        if (!showingChooser && !scannedOnce)
            scanForImages();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        passOnInvite(intent);
        Uri target = imageFromIntent(intent);
        if (target != null && !extracting)
            beginExtract(target, displayName(target));
    }

    /** an Xbox disc image opened or shared with the app */
    private Uri imageFromIntent(Intent intent) {
        if (intent == null)
            return null;
        String action = intent.getAction();
        Uri data = intent.getData();
        if (Intent.ACTION_SEND.equals(action)) {
            android.os.Parcelable shared = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            if (shared instanceof Uri)
                data = (Uri) shared;
        } else if (!Intent.ACTION_VIEW.equals(action)) {
            return null;
        }
        if (data == null || "halo".equals(data.getScheme()))
            return null;
        return data;
    }

    /**
     * An internet play invite link the app was opened with: the game
     * (port/linux/src/p2p.c) picks it up from join_link.txt, whether it is
     * starting now or already running.
     */
    private void passOnInvite(Intent intent) {
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction()) || intent.getData() == null
            || dataRoot == null || !"halo".equals(intent.getData().getScheme()))
            return;
        // written whole under another name, then renamed: the game never
        // reads it half written
        File partial = new File(dataRoot, "join_link.txt.tmp");
        try (OutputStream out = new FileOutputStream(partial)) {
            out.write(intent.getData().toString().getBytes("UTF-8"));
        } catch (IOException e) {
            // the link is lost; the player can copy it instead
            partial.delete();
            return;
        }
        if (!partial.renameTo(new File(dataRoot, "join_link.txt")))
            partial.delete();
    }

    /**
     * This device's ANDROID_ID (the app's own: one per app signing key and
     * user, until a factory reset), which native code cannot read: the game
     * (port/linux/src/p2p.c) hashes it from hardware_id.txt into the
     * hardware id a host it joins is told.
     */
    private void passOnHardwareId() {
        String id;

        if (dataRoot == null)
            return;
        try {
            id = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
        } catch (RuntimeException e) {
            return;
        }
        if (id == null || id.isEmpty())
            return;
        File partial = new File(dataRoot, "hardware_id.txt.tmp");
        try (OutputStream out = new FileOutputStream(partial)) {
            out.write(id.getBytes("UTF-8"));
        } catch (java.io.IOException e) {
            partial.delete();
            return;
        }
        if (!partial.renameTo(new File(dataRoot, "hardware_id.txt")))
            partial.delete();
    }

    private boolean haveData() {
        return dataRoot != null && new File(dataRoot, "maps/ui.map").isFile();
    }

    private void startGame() {
        startActivity(new Intent(this, HaloActivity.class));
        finish();
    }

    // ---------------------------------------------------------------- the screen

    private void buildInterface() {
        FrameLayout root = new FrameLayout(this);
        root.addView(new HaloBackgroundView(this),
            new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setClipToPadding(false);
        root.addView(scroll, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = HaloUi.dp(this, 40);
        column.setPadding(pad, HaloUi.dp(this, 30), pad, HaloUi.dp(this, 30));
        scroll.addView(column, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView classification = HaloUi.text(this, "UNSC // ONI SECTION III   ·   EYES ONLY", HaloUi.AMBER, 11,
            HaloUi.MONO);
        classification.setGravity(Gravity.CENTER);
        column.addView(classification);
        TextView title = HaloUi.heading(this, "Halo: Combat Evolved", HaloUi.TEXT, 30);
        title.setLetterSpacing(0.3f);
        title.setGravity(Gravity.CENTER);
        column.addView(title, margins(0, 6, 0, 0));
        TextView subtitle = HaloUi.text(this, "INSTALLATION 04   ·   ARCHIVE RECOVERY PROTOCOL", HaloUi.CYAN_DIM,
            12, HaloUi.MONO);
        subtitle.setGravity(Gravity.CENTER);
        column.addView(subtitle, margins(0, 2, 0, 0));

        View panel = buildPanel();
        panel.setBackground(HaloUi.panel(this));
        int panelPad = HaloUi.dp(this, 22);
        panel.setPadding(panelPad, HaloUi.dp(this, 18), panelPad, HaloUi.dp(this, 18));
        column.addView(panel, margins(0, 22, 0, 0));

        buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.VERTICAL);
        column.addView(buttons, margins(0, 20, 0, 0));

        TextView footer = HaloUi.text(this, "CORTANA   //   DATA RECOVERY   //   UNSC INFINITY", HaloUi.CYAN_DIM, 11,
            HaloUi.MONO);
        footer.setGravity(Gravity.CENTER);
        column.addView(footer, margins(0, 24, 0, 0));

        setContentView(root);
    }

    private View buildPanel() {
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);

        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        HaloGlyphView glyph = new HaloGlyphView(this);
        header.addView(glyph, new LinearLayout.LayoutParams(HaloUi.dp(this, 40), HaloUi.dp(this, 40)));
        status = HaloUi.heading(this, "Initializing", HaloUi.CYAN, 16);
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(0,
            ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        statusParams.leftMargin = HaloUi.dp(this, 14);
        header.addView(status, statusParams);
        seal = HaloUi.text(this, "", HaloUi.AMBER, 11, HaloUi.MONO);
        header.addView(seal);
        content.addView(header);

        readout = HaloUi.readout(this, "", HaloUi.TEXT_DIM, 12.5f);
        readout.setMaxLines(6);
        content.addView(readout, margins(0, 12, 0, 0));

        progress = new HaloProgressView(this);
        LinearLayout.LayoutParams progressParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, HaloUi.dp(this, 20));
        progressParams.topMargin = HaloUi.dp(this, 16);
        content.addView(progress, progressParams);
        progress.setVisibility(View.GONE);

        percent = HaloUi.text(this, "", COLOR_ACTIVE, 12, HaloUi.MONO);
        percent.setGravity(Gravity.END);
        content.addView(percent, margins(0, 6, 0, 0));
        percent.setVisibility(View.GONE);

        return content;
    }

    private LinearLayout.LayoutParams margins(int left, int top, int right, int bottom) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        params.setMargins(HaloUi.dp(this, left), HaloUi.dp(this, top), HaloUi.dp(this, right),
            HaloUi.dp(this, bottom));
        return params;
    }

    /** the one thing a player came here for: big, filled, first */
    private void addPrimaryButton(String label, Runnable action) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTypeface(HaloUi.DISPLAY_BOLD);
        button.setLetterSpacing(0.12f);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 21);
        button.setGravity(Gravity.CENTER);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setMinimumWidth(0);
        button.setTextColor(HaloUi.VOID);
        button.setStateListAnimator(null);
        button.setPadding(HaloUi.dp(this, 24), HaloUi.dp(this, 22), HaloUi.dp(this, 24), HaloUi.dp(this, 22));
        button.setBackground(HaloUi.primaryBackground(this));
        button.setOnClickListener(view -> action.run());
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        params.bottomMargin = HaloUi.dp(this, 10);
        buttons.addView(button, params);
    }

    /** a small icon button, for the things a player rarely needs */
    private void addIconButton(LinearLayout row, String glyph, String label, Runnable action) {
        Button button = new Button(this);
        button.setText(glyph + " " + label);
        button.setAllCaps(false);
        button.setTypeface(HaloUi.MONO);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        button.setGravity(Gravity.CENTER);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setMinimumWidth(0);
        button.setTextColor(HaloUi.CYAN);
        button.setStateListAnimator(null);
        button.setPadding(HaloUi.dp(this, 6), HaloUi.dp(this, 12), HaloUi.dp(this, 6), HaloUi.dp(this, 12));
        button.setBackground(HaloUi.entryBackground(this));
        button.setOnClickListener(view -> action.run());
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        params.rightMargin = HaloUi.dp(this, 8);
        row.addView(button, params);
    }

    private void addButton(String label, Runnable action) {
        Button button = new Button(this);
        button.setText(label);
        HaloUi.styleButton(this, button);
        button.setOnClickListener(view -> action.run());
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        params.bottomMargin = HaloUi.dp(this, 10);
        buttons.addView(button, params);
    }

    private void setStatus(String value) {
        status.setText(value);
    }

    private void log(String line) {
        lines.add(line);
        while (lines.size() > 6)
            lines.remove(0);
        StringBuilder builder = new StringBuilder();
        for (int i = 0; i < lines.size(); i++) {
            if (i > 0)
                builder.append('\n');
            builder.append(lines.get(i));
        }
        readout.setText(builder);
    }

    private void showProgress(boolean visible) {
        progress.setVisibility(visible ? View.VISIBLE : View.GONE);
        percent.setVisibility(visible ? View.VISIBLE : View.GONE);
    }

    // ---------------------------------------------------------------- the search

    /**
     * Looks for an Xbox disc image where the player would keep one: the
     * app's own folder (where it can be pushed), then the shared storage
     * (Downloads, Documents and the top level).
     */
    private void scanForImages() {
        if (extracting || scanning || buttons == null)
            return;
        scanning = true;
        setStatus("Scanning for archive image");
        new Thread(() -> {
            boolean access = hasArchiveAccess();
            Set<String> seen = new LinkedHashSet<>();
            List<File> found = new ArrayList<>();
            scan(new File(dataRoot.getAbsolutePath()), 3, seen, found);
            scan(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), 2, seen, found);
            scan(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS), 2, seen, found);
            scan(Environment.getExternalStorageDirectory(), 1, seen, found);
            // tell the Halo discs apart from any other ROM the player keeps
            List<Candidate> halo = new ArrayList<>();
            List<Candidate> other = new ArrayList<>();
            for (File image : found) {
                XisoExtractor.Info info = probe(image);
                Candidate candidate = new Candidate(image, info);
                if (info.halo)
                    halo.add(candidate);
                else
                    other.add(candidate);
            }
            sort(halo);
            sort(other);
            Log.i(TAG, "scan: " + found.size() + " image(s), " + halo.size()
                + " Xbox Halo disc(s), archive access=" + access);
            handler.post(() -> {
                scanning = false;
                presentScan(halo, other, access);
            });
        }, "archive-scan").start();
    }

    /** opens the image and asks XisoExtractor what it holds */
    private XisoExtractor.Info probe(File image) {
        FileChannel channel = null;
        try {
            channel = new java.io.RandomAccessFile(image, "r").getChannel();
            return XisoExtractor.probe(channel);
        } catch (Exception exception) {
            return new XisoExtractor.Info();
        } finally {
            close(channel);
        }
    }

    /** whole Halo discs first, then the fullest, then the biggest, then by name */
    private static void sort(List<Candidate> candidates) {
        candidates.sort((a, b) -> {
            if (a.info.complete != b.info.complete)
                return a.info.complete ? -1 : 1;
            if (a.info.mapCount != b.info.mapCount)
                return b.info.mapCount - a.info.mapCount;
            if (a.info.dataBytes != b.info.dataBytes)
                return Long.compare(b.info.dataBytes, a.info.dataBytes);
            return a.file.getName().compareToIgnoreCase(b.file.getName());
        });
    }

    private void scan(File directory, int depth, Set<String> seen, List<File> out) {
        if (directory == null || depth < 0)
            return;
        File[] children = directory.listFiles();
        if (children == null)
            return;
        for (File child : children) {
            if (child.isDirectory()) {
                if (depth > 0 && !child.getName().equals("Android"))
                    scan(child, depth - 1, seen, out);
                continue;
            }
            String name = child.getName().toLowerCase(Locale.US);
            if ((name.endsWith(".iso") || name.endsWith(".xiso")) && seen.add(child.getAbsolutePath()))
                out.add(child);
        }
    }

    private void presentScan(List<Candidate> halo, List<Candidate> other, boolean access) {
        if (extracting)
            return;
        haloImages.clear();
        haloImages.addAll(halo);
        otherImages.clear();
        otherImages.addAll(other);
        archiveAccess = access;
        buttons.removeAllViews();
        scannedOnce = true;
        saveKnownHalos(halo);

        if (!access)
            log("> deep scan locked: grant archive access to search shared storage");

        if (haveData())
            addButton("Play", this::startGame);

        File remembered = rememberedDisc();

        if (!halo.isEmpty()) {
            Candidate automatic = automatic(halo, remembered);
            if (autoExtractPending && automatic != null) {
                autoExtractPending = false;
                log("> opening " + automatic.file.getName());
                beginExtract(automatic.file, automatic.file.getName());
                return;
            }
            autoExtractPending = false;
            setStatus(halo.size() == 1 ? "Halo disc detected" : "Halo discs detected");
            log("> " + halo.size() + " Halo disc(s) identified");
            for (Candidate candidate : halo)
                buttons.addView(candidateRow(candidate, tagFor(candidate, halo, remembered)));
        } else if (!other.isEmpty()) {
            autoExtractPending = false;
            setStatus("No Halo disc found");
            log("> " + other.size() + " image(s) here are not Halo discs");
        } else {
            autoExtractPending = false;
            setStatus("No archive image found");
            log("> no .iso or .xiso in reach");
        }

        if (!other.isEmpty()) {
            sectionLabel("OTHER IMAGES");
            for (Candidate candidate : other)
                buttons.addView(candidateRow(candidate, null));
        }

        sectionLabel("ACTIONS");

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        buttons.addView(row, margins(0, 2, 0, 0));
        if (!access)
            addIconButton(row, "\u2302", "ACCESS", this::requestArchiveAccess);
        addIconButton(row, "\u25CE", "IMAGE", this::pickImage);
        addIconButton(row, "\u25A4", "FOLDER", this::pickFolder);
        addIconButton(row, "\u21BB", "SCAN", this::scanForImages);
    }

    /** the badge a candidate pane carries, if any */
    private String tagFor(Candidate candidate, List<Candidate> halo, File remembered) {
        if (haveData() && sameFile(candidate.file, remembered))
            return "INSTALLED";
        if (sameFile(candidate.file, remembered))
            return "LAST USED";
        if (candidate == halo.get(0))
            return "RECOMMENDED";
        return null;
    }

    /**
     * The disc to restore without asking: the one that worked last time,
     * else the only whole Halo disc. More than one whole disc is the
     * player's choice.
     */
    private Candidate automatic(List<Candidate> halo, File remembered) {
        if (remembered != null) {
            for (Candidate candidate : halo)
                if (sameFile(candidate.file, remembered) && candidate.info.complete)
                    return candidate;
        }
        Candidate complete = null;
        int count = 0;
        for (Candidate candidate : halo)
            if (candidate.info.complete) {
                complete = candidate;
                count++;
            }
        return count == 1 ? complete : null;
    }

    /** a disc image as a tappable pane: the name, and what probing found */
    private View candidateRow(Candidate candidate, String tag) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setBackground(HaloUi.entryBackground(this));
        int pad = HaloUi.dp(this, 16);
        row.setPadding(pad, HaloUi.dp(this, 12), pad, HaloUi.dp(this, 12));
        row.setClickable(true);
        row.setFocusable(true);

        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        TextView name = HaloUi.text(this, candidate.file.getName(), HaloUi.CYAN, 15, HaloUi.DISPLAY_BOLD);
        name.setLetterSpacing(0.06f);
        name.setSingleLine(true);
        name.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        top.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (tag != null)
            top.addView(HaloUi.text(this, tag, HaloUi.AMBER, 10, HaloUi.MONO));
        row.addView(top);
        row.addView(HaloUi.text(this, candidate.info.summary(), HaloUi.TEXT_DIM, 11.5f, HaloUi.MONO));
        row.setOnClickListener(view -> beginExtract(candidate.file, candidate.file.getName()));
        row.setLayoutParams(margins(0, 0, 0, 10));
        return row;
    }

    private void sectionLabel(String text) {
        TextView label = HaloUi.text(this, text, HaloUi.CYAN_DIM, 11, HaloUi.MONO);
        label.setLetterSpacing(0.22f);
        buttons.addView(label, margins(0, 8, 0, 8));
    }

    private File rememberedDisc() {
        String path = getSharedPreferences("halo-import", MODE_PRIVATE).getString("disc", null);
        File file = path != null ? new File(path) : null;
        return file != null && file.isFile() ? file : null;
    }

    private void rememberDisc(File file) {
        if (file == null)
            return;
        String path;
        try {
            path = file.getCanonicalPath();
        } catch (IOException exception) {
            path = file.getAbsolutePath();
        }
        getSharedPreferences("halo-import", MODE_PRIVATE).edit().putString("disc", path).apply();
    }

    /** the same disc, however the two paths spell it (/sdcard vs /storage/emulated/0) */
    private static boolean sameFile(File a, File b) {
        if (a == null || b == null)
            return false;
        if (a.equals(b))
            return true;
        try {
            return a.getCanonicalPath().equals(b.getCanonicalPath());
        } catch (IOException exception) {
            return false;
        }
    }

    /** remembers the Halo discs a scan found, so a later launch can ask cheaply */
    private void saveKnownHalos(List<Candidate> halo) {
        StringBuilder builder = new StringBuilder();
        for (Candidate candidate : halo) {
            if (builder.length() > 0)
                builder.append('\n');
            builder.append(pathOf(candidate.file));
        }
        getSharedPreferences("halo-import", MODE_PRIVATE).edit().putString("discs", builder.toString()).apply();
    }

    /** the remembered Halo discs that are still there, re-probed */
    private List<Candidate> knownHalos() {
        String stored = getSharedPreferences("halo-import", MODE_PRIVATE).getString("discs", "");
        List<Candidate> result = new ArrayList<>();
        Set<String> seen = new LinkedHashSet<>();
        for (String line : stored.split("\n")) {
            if (line.isEmpty() || !seen.add(line))
                continue;
            File file = new File(line);
            if (!file.isFile())
                continue;
            XisoExtractor.Info info = probe(file);
            if (info.halo)
                result.add(new Candidate(file, info));
        }
        sort(result);
        return result;
    }

    /** drops a disc that is gone from the remembered list */
    private void forgetKnown(File file) {
        if (file == null)
            return;
        String stored = getSharedPreferences("halo-import", MODE_PRIVATE).getString("discs", "");
        String target = pathOf(file);
        StringBuilder builder = new StringBuilder();
        for (String line : stored.split("\n")) {
            if (line.isEmpty() || line.equals(target))
                continue;
            if (builder.length() > 0)
                builder.append('\n');
            builder.append(line);
        }
        getSharedPreferences("halo-import", MODE_PRIVATE).edit().putString("discs", builder.toString()).apply();
    }

    private static String pathOf(File file) {
        try {
            return file.getCanonicalPath();
        } catch (IOException exception) {
            return file.getAbsolutePath();
        }
    }

    private static String trim(String value, int length) {
        return value.length() <= length ? value : value.substring(0, length - 1) + "…";
    }

    private boolean hasArchiveAccess() {
        if (Build.VERSION.SDK_INT >= 30)
            return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestArchiveAccess() {
        log("> requesting archive access");
        rescanOnResume = true;
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            } catch (Exception exception) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE }, REQUEST_STORAGE);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == REQUEST_STORAGE)
            scanForImages();
    }

    private void pickImage() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES,
            new String[] { "application/x-iso9660-image", "application/octet-stream" });
        startActivityForResult(intent, PICK_IMAGE);
    }

    private void pickFolder() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME },
            null, null, null)) {
            if (cursor != null && cursor.moveToFirst())
                return cursor.getString(0);
        } catch (Exception exception) {
            // fall through to the last path segment
        }
        String path = uri.getLastPathSegment();
        return path != null ? path : "disc image";
    }

    // ---------------------------------------------------------------- the recovery

    private void beginExtract(File image, String label) {
        beginExtract(image, null, label);
    }

    private void beginExtract(Uri image, String label) {
        File file = "file".equals(image.getScheme()) && image.getPath() != null
            ? new File(image.getPath()) : null;
        beginExtract(file, image, label);
    }

    private void beginExtract(File file, Uri uri, String label) {
        if (extracting)
            return;
        extracting = true;
        cancelled = false;
        currentImage = file;
        currentUri = uri;
        lastLoggedPercent = -1;
        buttons.removeAllViews();
        seal.setText("LIVE");
        seal.setTextColor(HaloUi.AMBER);
        setStatus("Reading archive");
        log("> " + trim(label, 40));
        Log.i(TAG, "extracting " + label);
        showProgress(true);
        progress.setProgress(0f);
        percent.setText("0%");

        new Thread(() -> {
            FileChannel channel = null;
            ParcelFileDescriptor descriptor = null;
            try {
                if (file != null) {
                    channel = new java.io.RandomAccessFile(file, "r").getChannel();
                } else {
                    descriptor = getContentResolver().openFileDescriptor(uri, "r");
                    if (descriptor == null)
                        throw new IOException("Could not open the disc image.");
                    channel = new FileInputStream(descriptor.getFileDescriptor()).getChannel();
                }
                XisoExtractor.extractMaps(channel, dataRoot, (name, done, total) -> {
                    if (cancelled)
                        return false;
                    handler.post(() -> report(name, done, total));
                    return true;
                });
                handler.post(this::finishExtract);
            } catch (Cancelled stop) {
                handler.post(() -> reset("Recovery halted by operator."));
            } catch (Exception exception) {
                Log.w(TAG, "extraction failed", exception);
                handler.post(() -> reset(exception.getMessage() != null ? exception.getMessage()
                    : exception.toString()));
            } finally {
                close(channel);
                close(descriptor);
            }
        }, "archive-recovery").start();
    }

    private void report(String name, long done, long total) {
        float fraction = total > 0 ? (float) done / total : 0f;
        progress.setProgress(fraction);
        int current = (int) (fraction * 100);
        percent.setText(current + "%");
        setStatus("Reading " + trim(name, 26).toUpperCase(Locale.US));
        if (current / 10 != lastLoggedPercent / 10) {
            lastLoggedPercent = current;
            log("> maps/" + name + "  " + current + "%");
        }
    }

    private void finishExtract() {
        extracting = false;
        seal.setText("SEALED");
        seal.setTextColor(HaloUi.CYAN);
        if (haveData()) {
            setStatus("Archive restored");
            log("> maps/ui.map verified");
            Log.i(TAG, "maps/ui.map verified; starting the game");
            rememberDisc(currentImage);
            offerReclaim(this::startGame);
        } else {
            reset("The copy finished but maps/ui.map is missing.");
        }
    }

    /**
     * Once the maps are safe, offers to remove the disc image: it is the
     * biggest thing the player just spent space on, and it is no longer
     * needed. Only worth asking when it is large.
     */
    private void offerReclaim(Runnable then) {
        final long bytes = sourceBytes();
        if (bytes < (1L << 30) || (currentImage == null && currentUri == null)) {
            then.run();
            return;
        }
        final Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(HaloUi.panel(this));
        int pad = HaloUi.dp(this, 24);
        panel.setPadding(pad, pad, pad, pad);
        panel.addView(HaloUi.heading(this, "Archive secured", HaloUi.CYAN, 17));
        panel.addView(HaloUi.readout(this, "maps/ is installed. Delete " + sourceName()
            + " to reclaim " + human(bytes) + "? The game data stays.", HaloUi.TEXT, 13), margins(0, 12, 0, 0));

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setGravity(Gravity.END);
        Button keep = new Button(this);
        keep.setText("Keep image");
        HaloUi.styleButton(this, keep);
        keep.setOnClickListener(view -> {
            dialog.dismiss();
            then.run();
        });
        Button delete = new Button(this);
        delete.setText("Delete image");
        HaloUi.styleButton(this, delete);
        delete.setOnClickListener(view -> {
            deleteSource();
            dialog.dismiss();
            then.run();
        });
        actions.addView(keep, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams deleteParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        deleteParams.leftMargin = HaloUi.dp(this, 12);
        actions.addView(delete, deleteParams);
        panel.addView(actions, margins(0, 18, 0, 0));

        dialog.setContentView(panel);
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(0));
            window.setLayout((int) (getResources().getDisplayMetrics().widthPixels * 0.82f),
                WindowManager.LayoutParams.WRAP_CONTENT);
        }
        dialog.setCancelable(false);
        dialog.show();
    }

    private void deleteSource() {
        String name = sourceName();
        try {
            boolean deleted;
            if (currentImage != null)
                deleted = currentImage.delete();
            else
                deleted = DocumentsContract.deleteDocument(getContentResolver(), currentUri);
            Log.i(TAG, "delete " + name + ": " + deleted);
            if (deleted) {
                getSharedPreferences("halo-import", MODE_PRIVATE).edit().remove("disc").apply();
                forgetKnown(currentImage);
                log("> disc image deleted; the maps are safe");
            } else {
                log("! could not delete " + name);
            }
        } catch (Exception exception) {
            Log.w(TAG, "delete failed", exception);
            log("! could not delete " + name);
        }
    }

    private long sourceBytes() {
        if (currentImage != null)
            return currentImage.length();
        if (currentUri != null) {
            try (Cursor cursor = getContentResolver().query(currentUri,
                new String[] { OpenableColumns.SIZE }, null, null, null)) {
                if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0))
                    return cursor.getLong(0);
            } catch (Exception exception) {
                // fall through to zero
            }
        }
        return 0;
    }

    private String sourceName() {
        if (currentImage != null)
            return currentImage.getName();
        if (currentUri != null)
            return displayName(currentUri);
        return "the disc image";
    }

    private static String human(long bytes) {
        if (bytes >= 1L << 30)
            return String.format(Locale.US, "%.1f GB", bytes / (double) (1L << 30));
        if (bytes >= 1L << 20)
            return String.format(Locale.US, "%.0f MB", bytes / (double) (1L << 20));
        return bytes + " B";
    }

    private static final class Cancelled extends IOException {
        Cancelled() {
            super("stopped");
        }
    }

    private void reset(String message) {
        extracting = false;
        seal.setText("HALTED");
        seal.setTextColor(HaloUi.AMBER);
        showProgress(false);
        setStatus("Recovery halted");
        log("! " + message);
        presentScan(haloImages, otherImages, archiveAccess);
    }

    private static void close(java.io.Closeable closeable) {
        if (closeable != null) {
            try {
                closeable.close();
            } catch (IOException ignored) {
                // best effort
            }
        }
    }

    @Override
    public void onBackPressed() {
        if (extracting) {
            cancelled = true;
            log("> halt requested…");
            return;
        }
        super.onBackPressed();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri uri = data.getData();
        if (requestCode == PICK_IMAGE) {
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception exception) {
                // some providers do not offer a persistable grant
            }
            beginExtract(uri, displayName(uri));
        } else if (requestCode == PICK_FOLDER) {
            extracting = true;
            cancelled = false;
            buttons.removeAllViews();
            seal.setText("LIVE");
            seal.setTextColor(HaloUi.AMBER);
            setStatus("Copying game data");
            showProgress(true);
            progress.setProgress(0f);
            new Thread(() -> importData(uri), "game-data-import").start();
        }
    }

    // ---------------------------------------------------------------- the maps folder

    /** the children of a document in the picked tree */
    private List<String[]> children(ContentResolver resolver, Uri tree, String documentId) {
        List<String[]> result = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId);
        String[] columns = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
        };
        try (Cursor cursor = resolver.query(uri, columns, null, null, null)) {
            while (cursor != null && cursor.moveToNext()) {
                result.add(new String[] {
                    cursor.getString(0), cursor.getString(1), cursor.getString(2),
                    cursor.isNull(3) ? "0" : cursor.getString(3),
                });
            }
        }
        return result;
    }

    private void collect(ContentResolver resolver, Uri tree, String documentId, String path, List<Entry> out) {
        for (String[] child : children(resolver, tree, documentId)) {
            String childPath = path.isEmpty() ? child[1] : path + "/" + child[1];
            if (DocumentsContract.Document.MIME_TYPE_DIR.equals(child[2]))
                collect(resolver, tree, child[0], childPath, out);
            else
                out.add(new Entry(DocumentsContract.buildDocumentUriUsingTree(tree, child[0]), childPath,
                    Long.parseLong(child[3])));
        }
    }

    private void importData(Uri tree) {
        try {
            ContentResolver resolver = getContentResolver();
            String rootId = DocumentsContract.getTreeDocumentId(tree);
            List<Entry> entries = new ArrayList<>();
            collect(resolver, tree, rootId, "", entries);

            // the picked folder holds maps/, or is maps/ itself
            boolean hasMapsFolder = false, isMapsFolder = false;
            for (Entry entry : entries) {
                if (entry.path.equalsIgnoreCase("maps/ui.map"))
                    hasMapsFolder = true;
                if (entry.path.equalsIgnoreCase("ui.map"))
                    isMapsFolder = true;
            }
            if (!hasMapsFolder && !isMapsFolder) {
                handler.post(() -> reset("That folder does not contain maps/ui.map."));
                return;
            }
            long total = 0, done = 0;
            for (Entry entry : entries)
                total += entry.size;
            byte[] buffer = new byte[1 << 20];
            for (Entry entry : entries) {
                if (cancelled)
                    throw new Cancelled();
                String path = isMapsFolder ? "maps/" + entry.path : entry.path;
                File destination = new File(dataRoot, path);
                File parent = destination.getParentFile();
                if (parent != null)
                    parent.mkdirs();
                File partial = new File(destination.getPath() + ".partial");
                try (InputStream in = resolver.openInputStream(entry.uri);
                     OutputStream out = new FileOutputStream(partial)) {
                    int count;
                    while ((count = in.read(buffer)) > 0) {
                        out.write(buffer, 0, count);
                        done += count;
                        final long copied = done;
                        final String copying = path;
                        final long all = total;
                        handler.post(() -> {
                            float fraction = all > 0 ? (float) copied / all : 0f;
                            progress.setProgress(fraction);
                            percent.setText((int) (fraction * 100) + "%");
                            setStatus("Copying " + trim(copying, 24).toUpperCase(Locale.US));
                        });
                    }
                }
                if (!partial.renameTo(destination))
                    throw new IOException("cannot write " + destination);
            }
            handler.post(() -> {
                extracting = false;
                if (haveData()) {
                    setStatus("Archive restored");
                    log("> maps/ui.map verified — starting game");
                    handler.postDelayed(this::startGame, 650);
                } else {
                    reset("The copy finished but maps/ui.map is missing.");
                }
            });
        } catch (Cancelled stop) {
            handler.post(() -> reset("Copy halted by operator."));
        } catch (Exception exception) {
            handler.post(() -> reset("Copying failed: " + exception.getMessage()));
        }
    }
}
