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

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
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
    static void mirrorToDownloads(Activity activity, File logFile, File dataRoot, String[] arguments) {
        if (logFile == null) return;
        final long started = System.currentTimeMillis();
        final File liveFile = liveFileFor(logFile);
        sLive = liveFile;
        StringBuilder settings = new StringBuilder();
        for (String argument : arguments) {
            if (argument.startsWith("--gta4_") || argument.startsWith("--android_")
                    || argument.startsWith("--log_level")) settings.append(argument).append(' ');
        }
        String build = "?";
        try {
            build = String.valueOf(activity.getPackageManager()
                    .getPackageInfo(activity.getPackageName(), 0).getLongVersionCode());
        } catch (Exception ignored) {
        }
        note("session: " + Build.MANUFACTURER + " " + Build.MODEL + " android=" + Build.VERSION.SDK_INT
                + " abi=" + Build.SUPPORTED_ABIS[0] + " build=" + build);
        note("settings: " + settings);
        final Mirror main = new Mirror(activity.getContentResolver(), logFile.getName());
        final Mirror runtime = new Mirror(activity.getContentResolver(), null);
        // 40 s of the final sound output, written by the game process a while after it starts: lets
        // choppy sound be analysed. A copy left by an earlier session is removed first.
        final File captureFile = new File(liveFile.getParentFile(), "audio-capture.wav");
        //noinspection ResultOfMethodCallIgnored
        captureFile.delete();
        final Mirror capture = new Mirror(activity.getContentResolver(),
                "audio-" + logFile.getName().replace(".log", ".wav"));
        activity.runOnUiThread(() -> Toast.makeText(activity,
                "Recording log to Downloads/LibertyRecomp", Toast.LENGTH_LONG).show());
        Thread thread = new Thread(() -> {
            java.io.OutputStream live;
            try {
                live = new FileOutputStream(liveFile, true);
            } catch (IOException e) {
                Log.w(TAG, "cannot open " + liveFile, e);
                return;
            }
            for (;;) {
                try {
                    String perf = perfLine(activity);
                    // A long-lived logcat child dies shortly after the game library loads
                    // on some phones (seen on Android 17), so the log is polled with short
                    // logcat -d calls from this thread instead and written from here.
                    pollLogcat(live, perf);
                    main.sync(liveFile);
                    if (captureFile.isFile()) capture.sync(captureFile);
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

    private static final java.util.LinkedHashSet<String> sSeen = new java.util.LinkedHashSet<>();
    private static int sEmptyPolls;

    /**
     * Appends the logcat lines not seen before, plus the perf line, to out. The dump is
     * the last 3000 lines each time and repeats are dropped by content, so it does not
     * depend on timestamp handling.
     */
    private static void pollLogcat(java.io.OutputStream out, String perf) {
        Process process = null;
        try {
            process = new ProcessBuilder("logcat", "-d", "-v", "threadtime", "-b", "main,system,crash",
                    "-t", "3000").redirectErrorStream(true).start();
            int read = 0, added = 0;
            List<String> other = new ArrayList<>();
            try (BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    if (line.isEmpty() || line.startsWith("---")) continue;
                    if (!Character.isDigit(line.charAt(0))) {
                        if (other.size() < 3) other.add(line);
                        continue;
                    }
                    ++read;
                    if (!sSeen.add(line)) continue;
                    ++added;
                    out.write((line + "\n").getBytes(StandardCharsets.UTF_8));
                }
            }
            int exit = process.waitFor();
            for (java.util.Iterator<String> it = sSeen.iterator(); sSeen.size() > 30000 && it.hasNext();) {
                it.next();
                it.remove();
            }
            sEmptyPolls = added == 0 ? sEmptyPolls + 1 : 0;
            String now = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.ROOT).format(new Date());
            out.write((now + " LibertyPerf: " + perf + "\n").getBytes(StandardCharsets.UTF_8));
            if (exit != 0 || !other.isEmpty() || (sEmptyPolls > 0 && sEmptyPolls % 6 == 3)) {
                out.write((now + " LibertyPoll: exit=" + exit + " read=" + read + " new=" + added
                        + " emptyPolls=" + sEmptyPolls + " other=" + other + "\n")
                        .getBytes(StandardCharsets.UTF_8));
            }
            out.flush();
        } catch (IOException | RuntimeException | InterruptedException e) {
            Log.w(TAG, "logcat poll failed", e);
        } finally {
            if (process != null) process.destroy();
        }
    }

    private static volatile File sLive;

    static File liveFileFor(File logFile) {
        return new File(logFile.getParentFile(), logFile.getName().replace(".log", "-live.log"));
    }

    /** One line straight into the debug log file; does nothing when no debug log is recording. */
    static synchronized void note(String text) {
        File live = sLive;
        if (live == null) return;
        String now = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.ROOT).format(new Date());
        try (FileOutputStream out = new FileOutputStream(live, true)) {
            out.write((now + " LibertyApp: " + text + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
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
        line.append(" threads=").append(threadLoad());
        return line.toString();
    }

    private static final Map<Integer, Long> sThreadTicks = new HashMap<>();
    private static long sThreadSampleNs;

    /** The six busiest threads since the last call, as name:percent of one core. */
    private static String threadLoad() {
        long now = System.nanoTime();
        double elapsedTicks = (now - sThreadSampleNs) / 1e7;  // 100 ticks per second
        boolean first = sThreadSampleNs == 0;
        sThreadSampleNs = now;
        File[] tasks = new File("/proc/self/task").listFiles();
        if (tasks == null || first) {
            if (tasks != null) remember(tasks);
            return "[]";
        }
        List<Object[]> busy = new ArrayList<>();
        Map<Integer, Long> next = new HashMap<>();
        for (File task : tasks) {
            String stat = read(new File(task, "stat"));
            int open = stat.indexOf('('), close = stat.lastIndexOf(')');
            if (open < 0 || close < open) continue;
            String[] fields = stat.substring(close + 2).split(" ");
            if (fields.length < 13) continue;
            int tid;
            long ticks;
            try {
                tid = Integer.parseInt(task.getName());
                ticks = Long.parseLong(fields[11]) + Long.parseLong(fields[12]);  // utime + stime
            } catch (NumberFormatException e) {
                continue;
            }
            next.put(tid, ticks);
            Long before = sThreadTicks.get(tid);
            if (before == null || ticks <= before) continue;
            busy.add(new Object[] {stat.substring(open + 1, close), 100.0 * (ticks - before) / elapsedTicks});
        }
        sThreadTicks.clear();
        sThreadTicks.putAll(next);
        busy.sort((a, b) -> Double.compare((Double) b[1], (Double) a[1]));
        StringBuilder out = new StringBuilder("[");
        for (int i = 0; i < busy.size() && i < 6; ++i) {
            if ((Double) busy.get(i)[1] < 3.0) break;
            out.append(i > 0 ? " " : "").append(busy.get(i)[0]).append(':')
                    .append(Math.round((Double) busy.get(i)[1])).append('%');
        }
        return out.append(']').toString();
    }

    private static void remember(File[] tasks) {
        sThreadTicks.clear();
        for (File task : tasks) {
            String stat = read(new File(task, "stat"));
            int close = stat.lastIndexOf(')');
            if (close < 0) continue;
            String[] fields = stat.substring(close + 2).split(" ");
            if (fields.length < 13) continue;
            try {
                sThreadTicks.put(Integer.parseInt(task.getName()),
                        Long.parseLong(fields[11]) + Long.parseLong(fields[12]));
            } catch (NumberFormatException ignored) {
            }
        }
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
