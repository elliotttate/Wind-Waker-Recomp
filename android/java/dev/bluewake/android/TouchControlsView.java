package dev.bluewake.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;

/**
 * GameCube touch controls drawn over the game: the control stick on the left,
 * A/B/X/Y on the right, L, R and Z at the top corners, START at the top and a
 * D-pad above the stick. Dragging anywhere else on the right half moves the
 * C-stick (the camera). The state goes to Aurora's virtual pad through
 * nativePublish (android/src/android_touch.c).
 *
 * Touches that land on no control fall through to the game's surface, so the
 * options menu (the Back gesture opens it) still works by touch.
 */
public class TouchControlsView extends View {
    // The iOS overlay's bits (android_touch.c).
    static final int DPAD_LEFT = 1, DPAD_RIGHT = 1 << 1, DPAD_DOWN = 1 << 2, DPAD_UP = 1 << 3;
    static final int Z = 1 << 4, R = 1 << 5, L = 1 << 6;
    static final int A = 1 << 8, B = 1 << 9, X = 1 << 10, Y = 1 << 11, START = 1 << 12;

    /** A tap shorter than this is held this long, so the game (30 reads a second) sees it. */
    private static final long MIN_HOLD_MS = 80;

    static native void nativePublish(int buttons, int stickX, int stickY, int cX, int cY);
    static native boolean nativeMenuOpen();

    private static final class Button {
        final int bit;
        final String label;
        final boolean round;
        final RectF rect = new RectF();
        Button(int bit, String label, boolean round) {
            this.bit = bit;
            this.label = label;
            this.round = round;
        }
        boolean hit(float x, float y, float slop) {
            return x >= rect.left - slop && x <= rect.right + slop && y >= rect.top - slop && y <= rect.bottom + slop;
        }
    }

    private final ArrayList<Button> buttons = new ArrayList<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float dp;
    private final Handler handler = new Handler(Looper.getMainLooper());

    // Stick geometry.
    private float stickCx, stickCy, stickRadius;
    // Which pointer drives what (-1: none).
    private int stickPointer = -1, cPointer = -1;
    private float cStartX, cStartY, cNowX, cNowY;
    private float stickX, stickY;           // -1..1, +y up
    private float cX, cY;
    private final int[] pointerBits = new int[32];   // by pointer id (Android's are 0-31)
    private final long[] pressedAt = new long[32];
    private int latched;                    // released too soon: held until MIN_HOLD_MS
    private final int[] latchGeneration = new int[16]; // by button: only the newest latch's timer ends it
    private int lastButtons, lastSx, lastSy, lastCx, lastCy;
    private boolean published;

    public TouchControlsView(Context context) {
        super(context);
        dp = context.getResources().getDisplayMetrics().density;
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(2 * dp);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        buttons.add(new Button(A, "A", true));
        buttons.add(new Button(B, "B", true));
        buttons.add(new Button(X, "X", true));
        buttons.add(new Button(Y, "Y", true));
        buttons.add(new Button(L, "L", false));
        buttons.add(new Button(R, "R", false));
        buttons.add(new Button(Z, "Z", false));
        buttons.add(new Button(START, "START", false));
        buttons.add(new Button(DPAD_UP, "▲", false));
        buttons.add(new Button(DPAD_DOWN, "▼", false));
        buttons.add(new Button(DPAD_LEFT, "◀", false));
        buttons.add(new Button(DPAD_RIGHT, "▶", false));
    }

    private Button find(int bit) {
        for (Button b : buttons) if (b.bit == bit) return b;
        return null;
    }

    private static void circle(RectF r, float cx, float cy, float radius) {
        r.set(cx - radius, cy - radius, cx + radius, cy + radius);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        // Everything in dp from the nearest edges, so the cover screen (about
        // 960x410 dp in landscape) and the inner one both fit.
        float s = Math.min(1f, Math.min(w, h) / (400 * dp));  // shrink on small screens
        float u = dp * s;
        stickRadius = 70 * u;
        stickCx = 115 * u;
        stickCy = h - 110 * u;
        float ax = w - 140 * u, ay = h - 130 * u;
        circle(find(A).rect, ax, ay, 40 * u);
        circle(find(B).rect, ax - 78 * u, ay + 45 * u, 28 * u);
        circle(find(X).rect, ax + 80 * u, ay - 10 * u, 28 * u);
        circle(find(Y).rect, ax - 12 * u, ay - 80 * u, 28 * u);
        find(L).rect.set(24 * u, 16 * u, 124 * u, 60 * u);
        find(R).rect.set(w - 124 * u, 16 * u, w - 24 * u, 60 * u);
        find(Z).rect.set(w - 110 * u, 70 * u, w - 38 * u, 108 * u);
        find(START).rect.set(w / 2f - 42 * u, 14 * u, w / 2f + 42 * u, 48 * u);
        float dx = 115 * u, dy = h - 265 * u, arm = 31 * u, half = 15 * u;
        if (dy - arm - half < 70 * u) dy = 70 * u + arm + half;  // clear of L
        find(DPAD_UP).rect.set(dx - half, dy - arm - half, dx + half, dy - arm + half);
        find(DPAD_DOWN).rect.set(dx - half, dy + arm - half, dx + half, dy + arm + half);
        find(DPAD_LEFT).rect.set(dx - arm - half, dy - half, dx - arm + half, dy + half);
        find(DPAD_RIGHT).rect.set(dx + arm - half, dy - half, dx + arm + half, dy + half);
        text.setTextSize(15 * u);
        invalidate();
    }

    private int heldButtons() {
        int bits = latched;
        for (int b : pointerBits) bits |= b;
        return bits;
    }

    private void publish() {
        int bits = heldButtons();
        int sx = Math.round(stickX * 127), sy = Math.round(stickY * 127);
        int cx = Math.round(cX * 127), cy = Math.round(cY * 127);
        if (published && bits == lastButtons && sx == lastSx && sy == lastSy && cx == lastCx && cy == lastCy)
            return;
        published = true;
        lastButtons = bits; lastSx = sx; lastSy = sy; lastCx = cx; lastCy = cy;
        try {
            nativePublish(bits, sx, sy, cx, cy);
        } catch (UnsatisfiedLinkError e) {
            // libmain.so not loaded yet.
        }
        invalidate();
    }

    /** Clears everything (the view hidden, the app paused). */
    public void reset() {
        stickPointer = cPointer = -1;
        stickX = stickY = cX = cY = 0;
        java.util.Arrays.fill(pointerBits, 0);
        latched = 0;
        handler.removeCallbacksAndMessages(null);
        published = false;
        publish();
    }

    private int buttonAt(float x, float y) {
        float slop = 6 * dp;
        for (Button b : buttons) if (b.hit(x, y, slop)) return b.bit;
        return 0;
    }

    private void press(int pointerId, int bits) {
        if (pointerId < 0 || pointerId >= pointerBits.length) return;
        int before = pointerBits[pointerId];
        int pressed = bits & ~before, released = before & ~bits;
        long now = SystemClock.uptimeMillis();
        for (int i = 0; i < 16; i++) {
            int bit = 1 << i;
            if ((pressed & bit) != 0) {
                pressedAt[i] = now;
                latched &= ~bit;
                latchGeneration[i]++;
            }
            if ((released & bit) != 0) {
                long held = now - pressedAt[i];
                if (held < MIN_HOLD_MS) {
                    latched |= bit;
                    final int unlatch = bit, button = i, generation = ++latchGeneration[i];
                    // An earlier tap's timer must not end this tap's latch early.
                    handler.postDelayed(() -> {
                        if (latchGeneration[button] != generation) return;
                        latched &= ~unlatch;
                        publish();
                    }, MIN_HOLD_MS - held);
                }
            }
        }
        pointerBits[pointerId] = bits;
    }

    private void updateStick(float x, float y) {
        float dx = (x - stickCx) / stickRadius, dy = (stickCy - y) / stickRadius;
        float m = (float) Math.hypot(dx, dy);
        if (m > 1) { dx /= m; dy /= m; }
        stickX = dx;
        stickY = dy;
    }

    private void updateC(float x, float y) {
        cNowX = x;
        cNowY = y;
        float range = 55 * dp;
        float dx = (x - cStartX) / range, dy = (cStartY - y) / range;
        float m = (float) Math.hypot(dx, dy);
        if (m > 1) { dx /= m; dy /= m; }
        cX = dx;
        cY = dy;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int index = e.getActionIndex();
        int id = e.getPointerId(index);
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = e.getX(index), y = e.getY(index);
                int bit = buttonAt(x, y);
                if (bit != 0) {
                    press(id, bit);
                } else if (stickPointer < 0 && Math.hypot(x - stickCx, y - stickCy) < stickRadius * 1.6f) {
                    stickPointer = id;
                    updateStick(x, y);
                } else if (cPointer < 0 && x > getWidth() / 2f && !nativeMenuOpen()) {
                    // The right half's blank area is the C-stick, except while the
                    // options menu is open: then it is the menu's, as the left is.
                    cPointer = id;
                    cStartX = x;
                    cStartY = y;
                    updateC(x, y);
                } else if (action == MotionEvent.ACTION_DOWN) {
                    return false;  // not ours: the game's surface (the menu) gets it
                }
                publish();
                return true;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int pid = e.getPointerId(i);
                    float x = e.getX(i), y = e.getY(i);
                    if (pid == stickPointer) updateStick(x, y);
                    else if (pid == cPointer) updateC(x, y);
                    else if (pid < pointerBits.length && pointerBits[pid] != 0) {
                        // Slide between buttons (A to B, along the D-pad).
                        int bit = buttonAt(x, y);
                        if (bit != 0 && bit != pointerBits[pid]) press(pid, bit);
                    }
                }
                publish();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (action == MotionEvent.ACTION_CANCEL) {
                    for (int i = 0; i < pointerBits.length; i++) press(i, 0);
                    stickPointer = cPointer = -1;
                    stickX = stickY = cX = cY = 0;
                } else {
                    if (id == stickPointer) { stickPointer = -1; stickX = stickY = 0; }
                    if (id == cPointer) { cPointer = -1; cX = cY = 0; }
                    press(id, 0);
                }
                publish();
                return true;
            }
            default:
                return true;
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        int held = heldButtons();
        // The stick.
        fill.setColor(0x33FFFFFF);
        stroke.setColor(0x88FFFFFF);
        canvas.drawCircle(stickCx, stickCy, stickRadius, fill);
        canvas.drawCircle(stickCx, stickCy, stickRadius, stroke);
        fill.setColor(stickPointer >= 0 ? 0xAAFFFFFF : 0x66FFFFFF);
        canvas.drawCircle(stickCx + stickX * stickRadius, stickCy - stickY * stickRadius, stickRadius * 0.45f, fill);
        // The C-stick drag, while one is active.
        if (cPointer >= 0) {
            fill.setColor(0x33FFD24A);
            canvas.drawCircle(cStartX, cStartY, 55 * dp, fill);
            fill.setColor(0x99FFD24A);
            canvas.drawCircle(cStartX + cX * 55 * dp, cStartY - cY * 55 * dp, 20 * dp, fill);
        }
        for (Button b : buttons) {
            boolean down = (held & b.bit) != 0;
            int color;
            switch (b.bit) {
                case A: color = 0x2EC4A0; break;   // GameCube green
                case B: color = 0xE0453A; break;   // red
                case START: color = 0xDDDDDD; break;
                default: color = 0xFFFFFF; break;
            }
            fill.setColor((down ? 0xAA000000 : 0x40000000) | color);
            stroke.setColor(0x99000000 | color);
            float radius = b.round ? b.rect.width() / 2 : 12 * dp;
            canvas.drawRoundRect(b.rect, radius, radius, fill);
            canvas.drawRoundRect(b.rect, radius, radius, stroke);
            text.setColor(down ? 0xFFFFFFFF : 0xCCFFFFFF);
            float ty = b.rect.centerY() - (text.descent() + text.ascent()) / 2;
            canvas.drawText(b.label, b.rect.centerX(), ty, text);
        }
    }
}
