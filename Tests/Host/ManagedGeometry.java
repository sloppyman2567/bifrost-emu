package org.bifrost.fixture;

import android.app.Activity;
import android.content.res.Configuration;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.webkit.ConsoleMessage;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.widget.FrameLayout;

/**
 * Window geometry and an attached WebView, reported from every point an app can
 * read it. The window is red; a magenta 40x40 dp marker sits in each corner of
 * the root view; the WebView fills the root with a green 100 CSS px div at its
 * top-left and a blue one at its bottom-right, so a screenshot shows where the
 * WebView, the root view and the host window edges land. GEOMETRY lines report
 * the Activity and Application resources, Display, WindowMetrics, Configuration,
 * the view tree and the page's own innerWidth/devicePixelRatio.
 */
public final class ManagedGeometry extends Activity {
    private final Handler main = new Handler();
    private FrameLayout root;
    private WebView web;
    private String page = "";

    private static String metrics(DisplayMetrics m) {
        return m.widthPixels + "x" + m.heightPixels + " density=" + m.density + " dpi=" + m.densityDpi;
    }

    private View marker(int gravity) {
        View view = new View(this);
        view.setBackgroundColor(Color.MAGENTA);
        float density = getResources().getDisplayMetrics().density;
        root.addView(view, new FrameLayout.LayoutParams((int) (40 * density), (int) (40 * density), gravity));
        return view;
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        root = new FrameLayout(this) {
            @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
                super.onSizeChanged(w, h, oldw, oldh);
                System.out.println("GEOMETRY root size " + w + "x" + h);
            }
        };
        root.setBackgroundColor(Color.RED);
        web = new WebView(this);
        web.getSettings().setJavaScriptEnabled(true);
        web.setWebChromeClient(new WebChromeClient() {
            @Override public boolean onConsoleMessage(ConsoleMessage message) {
                page = message.message();
                System.out.println("GEOMETRY " + page);
                return true;
            }
        });
        // A GL view as a game has: the sizes its renderer is given.
        android.opengl.GLSurfaceView gl = new android.opengl.GLSurfaceView(this);
        gl.setEGLContextClientVersion(3);
        gl.setRenderer(new android.opengl.GLSurfaceView.Renderer() {
            public void onSurfaceCreated(javax.microedition.khronos.opengles.GL10 g, javax.microedition.khronos.egl.EGLConfig c) { }
            public void onSurfaceChanged(javax.microedition.khronos.opengles.GL10 g, int w, int h) {
                System.out.println("GEOMETRY gl onSurfaceChanged " + w + "x" + h);
            }
            public void onDrawFrame(javax.microedition.khronos.opengles.GL10 g) { }
        });
        root.addView(gl, new FrameLayout.LayoutParams(-1, -1));
        root.addView(web, new FrameLayout.LayoutParams(-1, -1));
        marker(Gravity.TOP | Gravity.LEFT);
        marker(Gravity.TOP | Gravity.RIGHT);
        marker(Gravity.BOTTOM | Gravity.LEFT);
        marker(Gravity.BOTTOM | Gravity.RIGHT);
        setContentView(root);
        web.loadDataWithBaseURL("https://fixture.invalid/",
                "<html><head><meta name=viewport content='width=device-width,initial-scale=1'></head>"
                + "<body style='margin:0;background:#202020'>"
                + "<div style='position:fixed;left:0;top:0;width:100px;height:100px;background:#00ff00'></div>"
                + "<div style='position:fixed;right:0;bottom:0;width:100px;height:100px;background:#0000ff'></div>"
                + "<script>function r(){console.log('page inner=' + innerWidth + 'x' + innerHeight + ' dpr=' + devicePixelRatio"
                + " + ' screen=' + screen.width + 'x' + screen.height);} r(); addEventListener('resize', r);</script>"
                + "</body></html>", "text/html", "utf-8", null);
    }

    private void report(String tag) {
        android.view.Display display = getWindowManager().getDefaultDisplay();
        DisplayMetrics real = new DisplayMetrics(), current = new DisplayMetrics();
        display.getRealMetrics(real);
        display.getMetrics(current);
        Configuration config = getResources().getConfiguration();
        android.graphics.Rect bounds = getWindowManager().getCurrentWindowMetrics().getBounds();
        android.graphics.Rect max = getWindowManager().getMaximumWindowMetrics().getBounds();
        int[] location = new int[2];
        web.getLocationOnScreen(location);
        System.out.println("GEOMETRY " + tag + " activity=" + metrics(getResources().getDisplayMetrics())
                + " application=" + metrics(getApplicationContext().getResources().getDisplayMetrics())
                + " real=" + real.widthPixels + "x" + real.heightPixels + " displayMetrics=" + current.widthPixels + "x" + current.heightPixels
                + " window=" + bounds + " max=" + max
                + " config=" + config.screenWidthDp + "x" + config.screenHeightDp + "dp dpi=" + config.densityDpi
                + " decor=" + getWindow().getDecorView().getWidth() + "x" + getWindow().getDecorView().getHeight()
                + " root=" + root.getWidth() + "x" + root.getHeight()
                + " web=" + web.getWidth() + "x" + web.getHeight() + "@" + location[0] + "," + location[1]
                + " display=" + display.getWidth() + "x" + display.getHeight());
    }

    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        report("configuration-changed");
    }

    @Override public void onResume() {
        super.onResume();
        for (int i = 1; i <= 40; i++) main.postDelayed(() -> report("periodic"), i * 1500);
        main.postDelayed(() -> System.out.println("GEOMETRY_READY"), 4000);
        // A run for a person resizing the window can keep it open: BIFROST_GEOMETRY_SECONDS.
        main.postDelayed(this::finish, 1000L * Long.parseLong(System.getProperty("bifrost.geometry.seconds", "14")));
    }
}
