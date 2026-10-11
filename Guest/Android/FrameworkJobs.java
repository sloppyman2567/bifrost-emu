// Session JobScheduler: real JobService callbacks, namespaced queues and
// persisted jobs. Uses the virtual device's host network and power profile.
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.database.ContentObserver;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.PersistableBundle;
import android.os.Process;
import android.os.StatFs;
import android.os.SystemClock;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.function.BooleanSupplier;

final class FrameworkJobs {
    private final FrameworkPackages packages;
    private final BooleanSupplier interactive;
    private final Map<String, Map<Integer, Job>> queues = new LinkedHashMap<>();
    private Context context;
    private Handler main;
    private AtomicFile saved;
    private boolean closed;
    private int nextWorkId;
    private final Runnable tick = () -> pump();

    private final class Job implements ServiceConnection {
        final String namespace;
        final JobInfo info;
        final List<JobWorkItem> pending = new ArrayList<>();
        final Map<Integer, JobWorkItem> executing = new LinkedHashMap<>();
        long earliest, deadline;
        int failures;
        boolean running, bound, stopping, cancelled;
        long execution;
        Object service;
        JobParameters params;
        ContentObserver observer;
        final LinkedHashSet<Uri> changed = new LinkedHashSet<>();
        final LinkedHashSet<String> authorities = new LinkedHashSet<>();
        long firstChange;
        Job(String namespace, JobInfo info) {
            this.namespace = namespace; this.info = info;
            long now = SystemClock.elapsedRealtime();
            earliest = now + (info.isPeriodic() ? info.getIntervalMillis() - info.getFlexMillis()
                    : info.getMinLatencyMillis());
            deadline = !info.isPeriodic() && info.getMaxExecutionDelayMillis() > 0
                    ? now + info.getMaxExecutionDelayMillis() : Long.MAX_VALUE;
        }
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            synchronized (FrameworkJobs.this) {
                if (cancelled || !running) { release(this); return; }
                try {
                    service = Class.forName("android.app.job.IJobService$Stub")
                            .getMethod("asInterface", IBinder.class).invoke(null, binder);
                    params = parameters(this);
                    invokeService(this, "startJob");
                    JobParameters execution = params;
                    main.postDelayed(() -> { if (params == execution) timeout(this); }, 600000);
                } catch (Exception error) { failed(this, error); }
            }
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            synchronized (FrameworkJobs.this) { finish(this, true); }
        }
        @Override public void onBindingDied(ComponentName name) { onServiceDisconnected(name); }
        @Override public void onNullBinding(ComponentName name) {
            synchronized (FrameworkJobs.this) { failed(this, new IllegalStateException("Null JobService binding")); }
        }
    }

    FrameworkJobs(FrameworkPackages packages, BooleanSupplier interactive) {
        this.packages = packages; this.interactive = interactive;
    }
    void install(FrameworkServices services) throws Exception {
        services.register("jobscheduler", "android.app.job.IJobScheduler", this::call);
    }
    synchronized void attach(Context application) throws Exception {
        if (context != null) throw new IllegalStateException("JobScheduler already attached");
        context = application;
        main = new Handler(android.os.Looper.getMainLooper());
        saved = new AtomicFile(new File(context.getFilesDir(), ".asgard-jobs"));
        if (saved.getBaseFile().exists()) {
            Parcel p = Parcel.obtain();
            try {
                byte[] bytes = saved.readFully();
                if (bytes.length > 4 * 1024 * 1024) throw new IllegalStateException("Job store exceeds limit");
                p.unmarshall(bytes, 0, bytes.length); p.setDataPosition(0);
                if (p.readInt() != 1) throw new IllegalStateException("Unknown job store version");
                int count = p.readInt();
                if (count < 0 || count > 150) throw new IllegalStateException("Invalid job count");
                for (int i = 0; i < count; ++i) {
                    String ns = p.readString(); JobInfo info = p.readTypedObject(JobInfo.CREATOR);
                    long earliest = p.readLong(), deadline = p.readLong();
                    Job job = new Job(ns, info);
                    long offset = SystemClock.elapsedRealtime() - System.currentTimeMillis();
                    job.earliest = Math.max(SystemClock.elapsedRealtime(), earliest + offset);
                    job.deadline = deadline == Long.MAX_VALUE ? deadline : deadline + offset;
                    job.failures = p.readInt();
                    validate(info);
                    queue(ns).put(info.getId(), job);
                    observe(job);
                }
            } catch (Exception error) {
                for (Job job : all()) unobserve(job);
                queues.clear();
                android.util.Log.w("AsgardJobs", "Cannot restore persisted jobs", error);
            } finally { p.recycle(); }
        }
        wake();
    }

    private static <T extends Parcelable> T copy(T value, Parcelable.Creator<T> creator) {
        Parcel p = Parcel.obtain();
        try { value.writeToParcel(p, 0); p.setDataPosition(0); return creator.createFromParcel(p); }
        finally { p.recycle(); }
    }
    private Map<Integer, Job> queue(String namespace) {
        return queues.computeIfAbsent(namespace, unused -> new LinkedHashMap<>());
    }
    private Job find(String namespace, int id) {
        Map<Integer, Job> queue = queues.get(namespace);
        return queue == null ? null : queue.get(id);
    }
    private List<Job> all() {
        List<Job> result = new ArrayList<>();
        for (Map<Integer, Job> queue : queues.values()) result.addAll(queue.values());
        return result;
    }
    private static Object slice(List<?> list) throws Exception {
        return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class).newInstance(list);
    }
    private List<JobInfo> infos(String ns) {
        List<JobInfo> result = new ArrayList<>();
        Map<Integer, Job> queue = queues.get(ns);
        if (queue != null) for (Job job : queue.values()) result.add(copy(job.info, JobInfo.CREATOR));
        return result;
    }
    private void validate(JobInfo info) {
        if (context == null || closed) throw new IllegalStateException("JobScheduler is not accepting jobs");
        if (info == null) throw new IllegalArgumentException("null JobInfo");
        ServiceInfo service = packages.serviceInfo(info.getService(), PackageManager.GET_META_DATA, 0);
        if (service == null || service.applicationInfo.uid != Process.myUid())
            throw new IllegalArgumentException("JobService does not belong to the calling UID");
        if (!"android.permission.BIND_JOB_SERVICE".equals(service.permission))
            throw new IllegalArgumentException("JobService must require BIND_JOB_SERVICE");
        if (info.isPersisted() && context.checkSelfPermission("android.permission.RECEIVE_BOOT_COMPLETED")
                != PackageManager.PERMISSION_GRANTED)
            throw new SecurityException("Persisted jobs require RECEIVE_BOOT_COMPLETED");
    }
    private int schedule(String namespace, JobInfo info, JobWorkItem work) throws Exception {
        validate(info);
        if (namespace != null && (namespace.trim().isEmpty() || namespace.length() > 1000))
            throw new IllegalArgumentException("Invalid job namespace");
        // User-initiated transfers need the notification manager's visible
        // notification contract. This profile does not yet provide one.
        if (info.isUserInitiated()) return JobScheduler.RESULT_FAILURE;
        if (work != null && info.isPersisted())
            throw new IllegalArgumentException("Persisted work queues are not supported by this job store");
        if (work != null && work.getIntent() != null && (work.getIntent().getFlags() & 3) != 0)
            throw new SecurityException("Job work URI grants require a URI permission manager");
        Job previous = find(namespace, info.getId()), job;
        if (work != null && previous != null && previous.info.equals(info)) job = previous;
        else {
            if (previous == null && all().size() >= 150)
                throw new IllegalStateException("Calling UID has more than 150 jobs");
            job = new Job(namespace, copy(info, JobInfo.CREATOR));
            if (previous != null) cancel(previous);
            queue(namespace).put(info.getId(), job);
            observe(job);
        }
        if (work != null) {
            if (job.pending.size() + job.executing.size() >= 100) return JobScheduler.RESULT_FAILURE;
            JobWorkItem item = copy(work, JobWorkItem.CREATOR);
            JobWorkItem.class.getMethod("setWorkId", int.class).invoke(item, ++nextWorkId);
            job.pending.add(item);
        }
        persist(); wake();
        return JobScheduler.RESULT_SUCCESS;
    }
    private synchronized Object call(String method, Object[] a) throws Exception {
        if (Binder.getCallingUid() != Process.myUid()) throw new SecurityException("Foreign JobScheduler caller");
        switch (method) {
            case "schedule": return schedule((String) a[0], (JobInfo) a[1], null);
            case "enqueue":
                if (a[2] == null) throw new IllegalArgumentException("null JobWorkItem");
                return schedule((String) a[0], (JobInfo) a[1], (JobWorkItem) a[2]);
            case "cancel": {
                Job job = find((String) a[0], (Integer) a[1]); if (job != null) cancel(job);
                persist(); wake(); return null;
            }
            case "cancelAll":
            case "cancelAllInNamespace":
                for (Job job : all()) if (method.equals("cancelAll") || Objects.equals(job.namespace, a[0])) cancel(job);
                persist(); wake(); return null;
            case "getPendingJob": {
                Job job = find((String) a[0], (Integer) a[1]);
                return job == null ? null : copy(job.info, JobInfo.CREATOR);
            }
            case "getAllPendingJobsInNamespace": return slice(infos((String) a[0]));
            case "getAllPendingJobs": {
                Map<String, Object> result = new HashMap<>();
                for (String namespace : queues.keySet()) if (!queues.get(namespace).isEmpty())
                    result.put(namespace, slice(infos(namespace)));
                return result;
            }
            case "getPendingJobReason": {
                Job job = find((String) a[0], (Integer) a[1]);
                return job == null ? JobScheduler.PENDING_JOB_REASON_INVALID_JOB_ID : reason(job);
            }
            case "canRunUserInitiatedJobs":
            case "hasRunUserInitiatedJobsPermission":
                if (context == null || !context.getPackageName().equals(a[0])
                        || (a.length > 1 && (Integer) a[1] != 0)) throw new SecurityException("Foreign job package");
                return method.equals("hasRunUserInitiatedJobsPermission") &&
                        context.checkSelfPermission("android.permission.RUN_USER_INITIATED_JOBS") == PackageManager.PERMISSION_GRANTED;
            // These are system-only interfaces in AOSP JobSchedulerService.
            case "scheduleAsPackage":
            case "getStartedJobs":
            case "getAllJobSnapshots":
            case "registerUserVisibleJobObserver":
            case "unregisterUserVisibleJobObserver":
            case "notePendingUserRequestedAppStop": throw new SecurityException("Privileged JobScheduler operation: " + method);
            default: throw new UnsupportedOperationException("jobscheduler." + method);
        }
    }
    private Network network(Job job) {
        if (job.info.getRequiredNetwork() == null) return null;
        ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
        Network active = manager.getActiveNetwork();
        NetworkCapabilities caps = active == null ? null : manager.getNetworkCapabilities(active);
        return caps != null && job.info.getRequiredNetwork().canBeSatisfiedBy(caps) ? active : null;
    }
    private int reason(Job job) {
        if (job.running) return JobScheduler.PENDING_JOB_REASON_EXECUTING;
        long now = SystemClock.elapsedRealtime();
        if (job.info.getRequiredNetwork() != null && network(job) == null)
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY;
        if (job.info.isRequireStorageNotLow() && new StatFs(context.getFilesDir().getPath()).getAvailableBytes() < 64 * 1024 * 1024)
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_STORAGE_NOT_LOW;
        if (now >= job.deadline) return JobScheduler.PENDING_JOB_REASON_UNDEFINED;
        if (now < job.earliest) return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_MINIMUM_LATENCY;
        if (job.info.isRequireDeviceIdle() && interactive.getAsBoolean())
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_DEVICE_IDLE;
        if (job.info.getTriggerContentUris() != null && job.changed.isEmpty())
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER;
        if (job.info.isPrefetch()) return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_PREFETCH;
        // The virtual device is plugged in and has no discharging battery.
        return JobScheduler.PENDING_JOB_REASON_UNDEFINED;
    }
    private void wake() {
        if (main != null && !closed) { main.removeCallbacks(tick); main.post(tick); }
    }
    private void observe(Job job) {
        JobInfo.TriggerContentUri[] triggers = job.info.getTriggerContentUris();
        if (triggers == null) return;
        job.observer = new ContentObserver(main) {
            @Override public void onChange(boolean self, Uri uri) {
                synchronized (FrameworkJobs.this) {
                    if (job.cancelled || uri == null) return;
                    long now = SystemClock.elapsedRealtime();
                    if (job.changed.isEmpty()) job.firstChange = now;
                    if (job.changed.size() < 50) job.changed.add(uri);
                    if (uri.getAuthority() != null) job.authorities.add(uri.getAuthority());
                    long update = job.info.getTriggerContentUpdateDelay();
                    long maximum = job.info.getTriggerContentMaxDelay();
                    update = update < 0 ? 10000 : Math.max(500, update);
                    maximum = maximum < 0 ? 120000 : Math.max(1000, maximum);
                    job.earliest = Math.min(now + update, job.firstChange + maximum);
                    wake();
                }
            }
        };
        for (JobInfo.TriggerContentUri trigger : triggers) context.getContentResolver().registerContentObserver(
                trigger.getUri(), (trigger.getFlags() & JobInfo.TriggerContentUri.FLAG_NOTIFY_FOR_DESCENDANTS) != 0,
                job.observer);
    }
    private synchronized void pump() {
        if (closed) return;
        int running = 0;
        for (Job job : all()) if (job.running) running++;
        for (Job job : all()) {
            if (running >= 4) break;
            if (job.running || reason(job) != JobScheduler.PENDING_JOB_REASON_UNDEFINED) continue;
            job.running = true; running++;
            long execution = ++job.execution;
            try {
                job.bound = context.bindService(new Intent("android.app.job.JobService").setComponent(job.info.getService()),
                        job, Context.BIND_AUTO_CREATE);
                if (!job.bound) failed(job, new IllegalStateException("JobService could not be bound"));
                else main.postDelayed(() -> {
                    synchronized (FrameworkJobs.this) {
                        if (job.running && job.execution == execution && job.service == null)
                            failed(job, new IllegalStateException("JobService connection timed out"));
                    }
                }, 30000);
            } catch (Exception error) { failed(job, error); }
        }
        main.removeCallbacks(tick);
        if (!all().isEmpty()) main.postDelayed(tick, 1000);
    }
    private static void invokeService(Job job, String method) throws Exception {
        Class.forName("android.app.job.IJobService").getMethod(method, JobParameters.class).invoke(job.service, job.params);
    }
    private void timeout(Job job) {
        synchronized (this) {
            if (!job.running || job.cancelled || job.params == null) return;
            try {
                JobParameters.class.getMethod("setStopReason", int.class, int.class, String.class)
                        .invoke(job.params, JobParameters.STOP_REASON_TIMEOUT, 3, "Asgard execution timeout");
                job.stopping = true; invokeService(job, "stopJob");
                main.postDelayed(() -> { synchronized (FrameworkJobs.this) { if (job.stopping) finish(job, true); } }, 10000);
            } catch (Exception error) { failed(job, error); }
        }
    }
    private JobParameters parameters(Job job) throws Exception {
        IBinder callback = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws android.os.RemoteException {
                if (code == INTERFACE_TRANSACTION) { reply.writeString("android.app.job.IJobCallback"); return true; }
                data.enforceInterface("android.app.job.IJobCallback");
                if (Binder.getCallingUid() != Process.myUid()) throw new SecurityException("Foreign job callback");
                synchronized (FrameworkJobs.this) {
                    int id = data.readInt();
                    if (id != job.info.getId()) throw new IllegalArgumentException("Wrong callback job ID");
                    try {
                        String method = callbackName(code);
                        Object result = null;
                        switch (method) {
                            case "acknowledgeStartMessage": if (!data.readBoolean()) finish(job, false); break;
                            case "acknowledgeStopMessage":
                            case "jobFinished": finish(job, data.readBoolean()); break;
                            case "dequeueWork": {
                                if (!job.cancelled && !job.pending.isEmpty()) {
                                    JobWorkItem item = job.pending.remove(0);
                                    JobWorkItem.class.getMethod("bumpDeliveryCount").invoke(item);
                                    int workId = (Integer) JobWorkItem.class.getMethod("getWorkId").invoke(item);
                                    job.executing.put(workId, item); result = copy(item, JobWorkItem.CREATOR);
                                }
                                break;
                            }
                            case "completeWork": result = job.executing.remove(data.readInt()) != null; break;
                            case "acknowledgeGetTransferredDownloadBytesMessage":
                            case "acknowledgeGetTransferredUploadBytesMessage": data.readInt(); data.readLong(); break;
                            case "updateEstimatedNetworkBytes":
                            case "updateTransferredNetworkBytes": data.readTypedObject(JobWorkItem.CREATOR); data.readLong(); data.readLong(); break;
                            case "setNotification":
                                throw new UnsupportedOperationException("Job notifications require a notification manager");
                            default: return false;
                        }
                        data.enforceNoDataAvail(); reply.writeNoException();
                        if (method.equals("dequeueWork")) reply.writeTypedObject((JobWorkItem) result, 0);
                        if (method.equals("completeWork")) reply.writeBoolean((Boolean) result);
                        return true;
                    } catch (RuntimeException error) { throw error; }
                    catch (Exception error) { throw new IllegalStateException("Job callback", error); }
                }
            }
        };
        return JobParameters.class.getConstructor(IBinder.class, String.class, int.class,
                PersistableBundle.class, Bundle.class, android.content.ClipData.class, int.class,
                boolean.class, boolean.class, boolean.class, android.net.Uri[].class, String[].class, Network.class)
                .newInstance(callback, job.namespace, job.info.getId(), new PersistableBundle(job.info.getExtras()),
                        new Bundle(job.info.getTransientExtras()), job.info.getClipData(), job.info.getClipGrantFlags(),
                        SystemClock.elapsedRealtime() >= job.deadline, job.info.isExpedited(), false,
                        job.changed.isEmpty() ? null : job.changed.toArray(new Uri[0]),
                        job.authorities.isEmpty() ? null : job.authorities.toArray(new String[0]), network(job));
    }
    private static String callbackName(int code) throws Exception {
        Class<?> stub = Class.forName("android.app.job.IJobCallback$Stub");
        for (Field field : stub.getDeclaredFields()) if (field.getName().startsWith("TRANSACTION_")) {
            field.setAccessible(true);
            if (field.getInt(null) == code) return field.getName().substring(12);
        }
        return "unknown";
    }
    private void cancel(Job job) {
        if (find(job.namespace, job.info.getId()) == job) queue(job.namespace).remove(job.info.getId());
        job.cancelled = true;
        unobserve(job);
        if (job.service != null && job.params != null) {
            try {
                JobParameters.class.getMethod("setStopReason", int.class, int.class, String.class)
                        .invoke(job.params, JobParameters.STOP_REASON_CANCELLED_BY_APP, 0, "cancelled");
                job.stopping = true; invokeService(job, "stopJob");
            } catch (Exception error) { android.util.Log.w("AsgardJobs", "Stop callback failed", error); }
        }
        release(job);
    }
    private void release(Job job) {
        if (job.bound) {
            job.bound = false;
            try { context.unbindService(job); }
            catch (RuntimeException error) { android.util.Log.w("AsgardJobs", "Unbind failed", error); }
        }
        job.running = job.stopping = false; job.service = null; job.params = null;
    }
    private void unobserve(Job job) {
        if (job.observer != null) {
            context.getContentResolver().unregisterContentObserver(job.observer); job.observer = null;
        }
    }
    private void failed(Job job, Exception error) {
        android.util.Log.w("AsgardJobs", "JobService failed: " + job.info.getService(), error);
        finish(job, true);
    }
    private void finish(Job job, boolean reschedule) {
        if (!job.running) return;
        release(job);
        if (!job.cancelled && find(job.namespace, job.info.getId()) == job) {
            if (reschedule || !job.pending.isEmpty() || !job.executing.isEmpty()) {
                job.pending.addAll(0, job.executing.values()); job.executing.clear();
                int n = Math.min(20, ++job.failures);
                long initial = Math.max(10000, job.info.getInitialBackoffMillis());
                long backoff = job.info.getBackoffPolicy() == JobInfo.BACKOFF_POLICY_LINEAR ? initial * n : initial * (1L << (n - 1));
                job.earliest = SystemClock.elapsedRealtime() + Math.min(18000000, backoff);
                job.deadline = Long.MAX_VALUE;
            } else if (job.info.isPeriodic()) {
                job.failures = 0; job.earliest = SystemClock.elapsedRealtime() + job.info.getIntervalMillis();
            } else {
                queue(job.namespace).remove(job.info.getId());
                unobserve(job);
            }
        }
        persist(); wake();
    }
    private void persist() {
        if (saved == null) return;
        Parcel p = Parcel.obtain(); FileOutputStream output = null;
        try {
            List<Job> persisted = new ArrayList<>();
            for (Job job : all()) if (job.info.isPersisted()) persisted.add(job);
            p.writeInt(1); p.writeInt(persisted.size());
            long offset = System.currentTimeMillis() - SystemClock.elapsedRealtime();
            for (Job job : persisted) {
                p.writeString(job.namespace); p.writeTypedObject(job.info, 0);
                p.writeLong(job.earliest + offset);
                p.writeLong(job.deadline == Long.MAX_VALUE ? job.deadline : job.deadline + offset);
                p.writeInt(job.failures);
            }
            output = saved.startWrite(); output.write(p.marshall()); saved.finishWrite(output);
        } catch (Exception error) {
            if (output != null) saved.failWrite(output);
            android.util.Log.w("AsgardJobs", "Cannot persist jobs", error);
        } finally { p.recycle(); }
    }
    synchronized void close() {
        closed = true;
        if (main != null) main.removeCallbacks(tick);
        // Persist before tearing down services: app shutdown is not cancelAll.
        persist();
        for (Job job : all()) { job.cancelled = true; unobserve(job); release(job); }
    }
}
