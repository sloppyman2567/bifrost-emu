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

    private boolean gles1Checked;

    private static void expect(boolean condition, String what) {
        if (!condition) throw new AssertionError("OpenGL ES 1.x OES: " + what);
    }

    // The OES names of OpenGL ES 1.x apps, called as an app does (GLES11Ext, GLES10) against this context: they must change
    // real GL state and not just exist. glDrawTexfOES and glColor4f have no ES 2 equivalent: they return, and say so on stderr.
    private static void gles1Oes(int width, int height) {
        int[] id = new int[1];
        // A framebuffer made, completed and drawn into through the OES names.
        android.opengl.GLES11Ext.glGenFramebuffersOES(1, id, 0);
        int framebuffer = id[0];
        expect(framebuffer != 0, "framebuffer name");
        expect(!android.opengl.GLES11Ext.glIsFramebufferOES(framebuffer), "framebuffer exists before its first bind");
        android.opengl.GLES11Ext.glBindFramebufferOES(android.opengl.GLES11Ext.GL_FRAMEBUFFER_OES, framebuffer);
        expect(android.opengl.GLES11Ext.glIsFramebufferOES(framebuffer), "bound framebuffer is an object");
        android.opengl.GLES20.glGenTextures(1, id, 0);
        int texture = id[0];
        android.opengl.GLES20.glBindTexture(android.opengl.GLES20.GL_TEXTURE_2D, texture);
        android.opengl.GLES20.glTexImage2D(android.opengl.GLES20.GL_TEXTURE_2D, 0, android.opengl.GLES20.GL_RGBA, 4, 4, 0, android.opengl.GLES20.GL_RGBA, android.opengl.GLES20.GL_UNSIGNED_BYTE, null);
        android.opengl.GLES11Ext.glFramebufferTexture2DOES(android.opengl.GLES11Ext.GL_FRAMEBUFFER_OES, android.opengl.GLES11Ext.GL_COLOR_ATTACHMENT0_OES, android.opengl.GLES20.GL_TEXTURE_2D, texture, 0);
        expect(android.opengl.GLES11Ext.glCheckFramebufferStatusOES(android.opengl.GLES11Ext.GL_FRAMEBUFFER_OES) == android.opengl.GLES11Ext.GL_FRAMEBUFFER_COMPLETE_OES,
                "framebuffer complete");
        android.opengl.GLES20.glViewport(0, 0, 4, 4);
        android.opengl.GLES20.glClearColor(1f, 0f, 0f, 1f);
        android.opengl.GLES20.glClear(android.opengl.GLES20.GL_COLOR_BUFFER_BIT);
        java.nio.ByteBuffer pixel = java.nio.ByteBuffer.allocateDirect(4);
        android.opengl.GLES20.glReadPixels(1, 1, 1, 1, android.opengl.GLES20.GL_RGBA, android.opengl.GLES20.GL_UNSIGNED_BYTE, pixel);
        expect((pixel.get(0) & 0xff) == 255 && pixel.get(1) == 0 && pixel.get(2) == 0, "cleared pixel is red");
        android.opengl.GLES11Ext.glBindFramebufferOES(android.opengl.GLES11Ext.GL_FRAMEBUFFER_OES, 0);
        id[0] = framebuffer;
        android.opengl.GLES11Ext.glDeleteFramebuffersOES(1, id, 0);
        expect(!android.opengl.GLES11Ext.glIsFramebufferOES(framebuffer), "deleted framebuffer is gone");
        android.opengl.GLES20.glViewport(0, 0, width, height);
        // A renderbuffer's storage, read back.
        android.opengl.GLES11Ext.glGenRenderbuffersOES(1, id, 0);
        int renderbuffer = id[0];
        expect(renderbuffer != 0, "renderbuffer name");
        android.opengl.GLES11Ext.glBindRenderbufferOES(android.opengl.GLES11Ext.GL_RENDERBUFFER_OES, renderbuffer);
        android.opengl.GLES11Ext.glRenderbufferStorageOES(android.opengl.GLES11Ext.GL_RENDERBUFFER_OES, android.opengl.GLES11Ext.GL_RGBA4_OES, 8, 6);
        int[] size = new int[2];
        android.opengl.GLES11Ext.glGetRenderbufferParameterivOES(android.opengl.GLES11Ext.GL_RENDERBUFFER_OES, android.opengl.GLES11Ext.GL_RENDERBUFFER_WIDTH_OES, size, 0);
        android.opengl.GLES11Ext.glGetRenderbufferParameterivOES(android.opengl.GLES11Ext.GL_RENDERBUFFER_OES, android.opengl.GLES11Ext.GL_RENDERBUFFER_HEIGHT_OES, size, 1);
        expect(size[0] == 8 && size[1] == 6, "renderbuffer size " + size[0] + "x" + size[1]);
        android.opengl.GLES11Ext.glBindRenderbufferOES(android.opengl.GLES11Ext.GL_RENDERBUFFER_OES, 0);
        id[0] = renderbuffer;
        android.opengl.GLES11Ext.glDeleteRenderbuffersOES(1, id, 0);
        // The blend state the OES names set.
        int[] state = new int[1];
        android.opengl.GLES11Ext.glBlendEquationOES(android.opengl.GLES11Ext.GL_FUNC_SUBTRACT_OES);
        android.opengl.GLES20.glGetIntegerv(android.opengl.GLES20.GL_BLEND_EQUATION_RGB, state, 0);
        expect(state[0] == android.opengl.GLES11Ext.GL_FUNC_SUBTRACT_OES, "blend equation " + state[0]);
        android.opengl.GLES11Ext.glBlendEquationSeparateOES(android.opengl.GLES11Ext.GL_FUNC_REVERSE_SUBTRACT_OES, android.opengl.GLES11Ext.GL_FUNC_ADD_OES);
        android.opengl.GLES20.glGetIntegerv(android.opengl.GLES20.GL_BLEND_EQUATION_RGB, state, 0);
        expect(state[0] == android.opengl.GLES11Ext.GL_FUNC_REVERSE_SUBTRACT_OES, "separate colour equation " + state[0]);
        android.opengl.GLES20.glGetIntegerv(android.opengl.GLES20.GL_BLEND_EQUATION_ALPHA, state, 0);
        expect(state[0] == android.opengl.GLES11Ext.GL_FUNC_ADD_OES, "separate alpha equation " + state[0]);
        android.opengl.GLES11Ext.glBlendFuncSeparateOES(android.opengl.GLES20.GL_ONE, android.opengl.GLES20.GL_ZERO, android.opengl.GLES20.GL_ZERO, android.opengl.GLES20.GL_ONE);
        int[] factors = {android.opengl.GLES20.GL_BLEND_SRC_RGB, android.opengl.GLES20.GL_BLEND_DST_RGB, android.opengl.GLES20.GL_BLEND_SRC_ALPHA, android.opengl.GLES20.GL_BLEND_DST_ALPHA};
        int[] expected = {android.opengl.GLES20.GL_ONE, android.opengl.GLES20.GL_ZERO, android.opengl.GLES20.GL_ZERO, android.opengl.GLES20.GL_ONE};
        for (int i = 0; i < 4; i++) {
            android.opengl.GLES20.glGetIntegerv(factors[i], state, 0);
            expect(state[0] == expected[i], "blend factor " + i + " = " + state[0]);
        }
        android.opengl.GLES11Ext.glBlendEquationOES(android.opengl.GLES11Ext.GL_FUNC_ADD_OES);
        android.opengl.GLES20.glBlendFunc(android.opengl.GLES20.GL_ONE, android.opengl.GLES20.GL_ZERO);
        // No ES 1 context: these return, and the first of them says so on stderr.
        android.opengl.GLES11Ext.glDrawTexfOES(0f, 0f, 0f, 1f, 1f);
        android.opengl.GLES10.glColor4f(1f, 1f, 1f, 1f);
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
                if (!gles1Checked) {
                    gles1Checked = true;
                    gles1Oes(w, h);
                    System.out.println("GEOMETRY gles1-oes ok");
                }
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
