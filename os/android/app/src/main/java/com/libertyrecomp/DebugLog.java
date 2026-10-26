package com.libertyrecomp;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.PowerManager;
import android.provider.MediaStore;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;

/**
 * "Play (Debug Log)": the game process records its own logcat - the runtime,
 * the Vulkan driver, SDL and the crash buffer - into
 * files/debug_logs/debug-DATE.log, so a player can attach one file to an
 * issue. Apps can read only their own logs, which is exactly what is wanted.
 *
 * <p>logcat runs as a child of the game process and would outlive a crash,
 * so a leftover writer is ended before the next game start and whenever the
 * launcher comes back; the file is size-capped either way.</p>
 */
final class DebugLog {

    private static final String TAG = "LibertyDebugLog";
    static final String DIRECTORY = "debug_logs";
    private static final int KEEP = 5;
    /** logcat -r is in kilobytes; with -n 1 at most twice this on disk. */
    private static final String ROTATE_KB = "8192";

    private DebugLog() {}

    static File directory(File dataRoot) {
        return new File(dataRoot, DIRECTORY);
    }

    /** Starts recording; returns the log file, or null if logcat could not start. */
    static File start(File dataRoot) {
        stopLeftovers(dataRoot);
        File dir = directory(dataRoot);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            Log.e(TAG, "cannot create " + dir);
            return null;
        }
        prune(dir);
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
        File file = new File(dir, "debug-" + stamp + ".log");
        try {
            new ProcessBuilder("logcat", "-v", "threadtime", "-b", "main,system,crash", "-T", "500",
                    "-f", file.getAbsolutePath(), "-r", ROTATE_KB, "-n", "1")
                    .redirectErrorStream(true)
                    .start();
            Log.i(TAG, "recording the game log to " + file);
            return file;
        } catch (IOException e) {
            Log.e(TAG, "logcat could not start", e);
            return null;
        }
    }

    /**
     * Copies the recorded log, and the game's own runtime log (which carries the
     * draw counts), into Downloads/LibertyRecomp as it grows, and adds a
     * "LibertyPerf" line every 5 seconds with the screen refresh rate, thermal
     * state, battery temperature and memory. Downloads stays readable after the
     * app is closed or uninstalled, and needs no storage permission (MediaStore).
     */
    static void mirrorToDownloads(Activity activity, File logFile, File dataRoot) {
        if (logFile == null) return;
        final long started = System.currentTimeMillis();
        final Mirror main = new Mirror(activity.getContentResolver(), logFile.getName());
        final Mirror runtime = new Mirror(activity.getContentResolver(), null);
        activity.runOnUiThread(() -> Toast.makeText(activity,
                "Recording log to Downloads/LibertyRecomp", Toast.LENGTH_LONG).show());
        Thread thread = new Thread(() -> {
            for (;;) {
                try {
                    Log.w("LibertyPerf", perfLine(activity));
                    main.sync(logFile);
                    File game = newestRuntimeLog(new File(dataRoot, "Liberty Recompiled/logs"), started);
                    if (game != null) {
                        runtime.rename("runtime-" + logFile.getName());
                        runtime.sync(game);
                    }
                    Thread.sleep(5000);
                } catch (InterruptedException e) {
                    return;
                } catch (RuntimeException e) {
                    Log.w(TAG, "downloads mirror", e);
                }
            }
        }, "DebugLogMirror");
        thread.setDaemon(true);
        thread.start();
    }

    private static File newestRuntimeLog(File dir, long since) {
        File[] files = dir.listFiles((d, n) -> n.endsWith(".log"));
        if (files == null) return null;
        File newest = null;
        for (File file : files) {
            if (file.lastModified() < since - 60000) continue;
            if (newest == null || file.lastModified() > newest.lastModified()) newest = file;
        }
        return newest;
    }

    private static String perfLine(Activity activity) {
        StringBuilder line = new StringBuilder("perf:");
        try {
            android.view.Display display = activity.getWindowManager().getDefaultDisplay();
            line.append(" refresh=").append(display.getRefreshRate()).append("Hz");
        } catch (RuntimeException ignored) {
        }
        try {
            PowerManager power = (PowerManager) activity.getSystemService(Activity.POWER_SERVICE);
            line.append(" thermal-status=").append(power.getCurrentThermalStatus());
            if (Build.VERSION.SDK_INT >= 30) {
                line.append(" thermal-headroom-10s=").append(power.getThermalHeadroom(10));
            }
        } catch (RuntimeException ignored) {
        }
        try {
            Intent battery = activity.registerReceiver(null,
                    new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (battery != null) {
                line.append(" battery=").append(battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0f)
                        .append("C level=").append(battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1))
                        .append("% plugged=").append(battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0));
            }
        } catch (RuntimeException ignored) {
        }
        android.os.Debug.MemoryInfo memory = new android.os.Debug.MemoryInfo();
        android.os.Debug.getMemoryInfo(memory);
        line.append(" pss=").append(memory.getTotalPss() / 1024).append("MB");
        return line.toString();
    }

    /** Appends a growing file to one Downloads/LibertyRecomp entry. */
    private static final class Mirror {
        private final ContentResolver resolver;
        private String name;
        private Uri uri;
        private long offset;

        Mirror(ContentResolver resolver, String name) {
            this.resolver = resolver;
            this.name = name;
        }

        void rename(String newName) {
            if (name == null) name = newName;
        }

        void sync(File source) {
            long length = source.length();
            if (name == null || length == offset) return;
            if (length < offset) offset = 0;  // logcat rotated: keep appending the new part
            try {
                if (uri == null) {
                    ContentValues values = new ContentValues();
                    values.put(MediaStore.Downloads.DISPLAY_NAME, name);
                    values.put(MediaStore.Downloads.MIME_TYPE, "text/plain");
                    values.put(MediaStore.Downloads.RELATIVE_PATH, "Download/LibertyRecomp");
                    uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
                    if (uri == null) return;
                }
                try (RandomAccessFile in = new RandomAccessFile(source, "r");
                     OutputStream out = resolver.openOutputStream(uri, "wa")) {
                    if (out == null) return;
                    in.seek(offset);
                    byte[] buffer = new byte[65536];
                    long remaining = length - offset;
                    while (remaining > 0) {
                        int read = in.read(buffer, 0, (int) Math.min(buffer.length, remaining));
                        if (read <= 0) break;
                        out.write(buffer, 0, read);
                        remaining -= read;
                        offset += read;
                    }
                }
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "copy to Downloads failed", e);
            }
        }
    }

    /** Ends logcat writers into debug_logs left over from an earlier session. */
    static void stopLeftovers(File dataRoot) {
        String marker = new File(dataRoot, DIRECTORY).getAbsolutePath();
        File[] processes = new File("/proc").listFiles();
        if (processes == null) return;
        for (File process : processes) {
            String name = process.getName();
            if (name.isEmpty() || !Character.isDigit(name.charAt(0))) continue;
            String commandLine = read(new File(process, "cmdline"));
            if (!commandLine.startsWith("logcat") || !commandLine.contains(marker)) continue;
            try {
                android.os.Process.sendSignal(Integer.parseInt(name), android.os.Process.SIGNAL_KILL);
                Log.i(TAG, "ended leftover log writer " + name);
            } catch (RuntimeException ignored) {
            }
        }
    }

    private static void prune(File dir) {
        File[] files = dir.listFiles((d, n) -> n.startsWith("debug-"));
        if (files == null || files.length < KEEP) return;
        Arrays.sort(files);
        for (int i = 0; i <= files.length - KEEP; ++i) {
            //noinspection ResultOfMethodCallIgnored
            files[i].delete();
        }
    }

    private static String read(File file) {
        try (InputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[4096];
            int length = in.read(buffer);
            if (length <= 0) return "";
            return new String(buffer, 0, length, StandardCharsets.UTF_8).replace('\0', ' ');
        } catch (IOException e) {
            return "";
        }
    }
}
