package com.halo.decomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.util.AttributeSet;
import android.view.View;

import java.util.Random;

/**
 * The room behind the terminal: a Halo ring arcing across a starfield, faint
 * scanlines, a slow sensor sweep and a vignette. Drawn, not an image, so it
 * scales to any display and drifts gently.
 */
public class HaloBackgroundView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF oval = new RectF();

    private Paint background;
    private Paint scanlines;
    private Paint vignette;
    private Paint sweep;
    private Paint ringGlow;
    private Paint ringBand;
    private Paint ringInner;
    private Paint ringOuter;
    private Paint starPaint;

    private float[] stars;      // x, y, radius, phase, speed
    private long start;
    private boolean running;

    public HaloBackgroundView(Context context) {
        super(context);
        init();
    }

    public HaloBackgroundView(Context context, AttributeSet attributes) {
        super(context, attributes);
        init();
    }

    private void init() {
        background = new Paint(Paint.ANTI_ALIAS_FLAG);
        vignette = new Paint(Paint.ANTI_ALIAS_FLAG);
        starPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        ringGlow = stroke(0x2A2E8AA6, 0);
        ringBand = stroke(0xCC0A1F2B, 0);
        ringInner = stroke(0x8A7EE7FF, 0);
        ringOuter = stroke(0x407EE7FF, 0);
        initScanlines();
        initStars();
    }

    private Paint stroke(int color, float width) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setStyle(Paint.Style.STROKE);
        paint.setColor(color);
        paint.setStrokeWidth(width);
        return paint;
    }

    private void initScanlines() {
        Bitmap tile = Bitmap.createBitmap(1, 4, Bitmap.Config.ARGB_8888);
        tile.setPixel(0, 0, 0x00000000);
        tile.setPixel(0, 1, 0x14000000);
        tile.setPixel(0, 2, 0x1E000000);
        tile.setPixel(0, 3, 0x14000000);
        scanlines = new Paint();
        scanlines.setShader(new BitmapShader(tile, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT));
    }

    private void initStars() {
        Random random = new Random(0x48414C4F5F3034L);  // Halo 04
        int count = 150;
        stars = new float[count * 5];
        for (int i = 0; i < count; i++) {
            stars[i * 5] = random.nextFloat();
            stars[i * 5 + 1] = random.nextFloat();
            stars[i * 5 + 2] = 0.4f + random.nextFloat() * 1.3f;
            stars[i * 5 + 3] = random.nextFloat() * 6.283f;
            stars[i * 5 + 4] = 0.4f + random.nextFloat() * 1.6f;
        }
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        background.setShader(new RadialGradient(width * 0.5f, height * 0.34f,
            Math.max(width, height) * 0.86f, new int[] { 0xFF0A2430, 0xFF04121A, HaloUi.VOID },
            new float[] { 0f, 0.45f, 1f }, Shader.TileMode.CLAMP));
        vignette.setShader(new RadialGradient(width * 0.5f, height * 0.5f,
            Math.max(width, height) * 0.72f, new int[] { 0x00000000, 0x00000000, 0xCC000000 },
            new float[] { 0f, 0.55f, 1f }, Shader.TileMode.CLAMP));
        if (!running)
            start = System.nanoTime();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        float width = getWidth();
        float height = getHeight();
        if (width <= 0 || height <= 0)
            return;
        float time = (System.nanoTime() - start) / 1_000_000_000f;

        canvas.drawRect(0, 0, width, height, background);

        // stars, breathing in and out
        for (int i = 0; i < stars.length; i += 5) {
            float alpha = 0.35f + 0.65f * (0.5f + 0.5f * (float) Math.sin(time * stars[i + 4] + stars[i + 3]));
            starPaint.setColor(0xFFCFEFFF);
            starPaint.setAlpha((int) (alpha * (stars[i + 2] * 120)));
            canvas.drawCircle(stars[i] * width, stars[i + 1] * height * 0.9f, stars[i + 2], starPaint);
        }

        drawRing(canvas, width, height, time);
        drawSweep(canvas, width, height, time);

        canvas.drawRect(0, 0, width, height, scanlines);
        canvas.drawRect(0, 0, width, height, vignette);

        if (isShown())
            postInvalidateOnAnimation();
    }

    /** the ring, seen from below: its far limb sweeps across the sky */
    private void drawRing(Canvas canvas, float width, float height, float time) {
        float cx = width * 0.5f;
        float cy = height * 1.16f + (float) Math.sin(time * 0.05f) * height * 0.006f;
        float rx = width * 0.86f;
        float ry = height * 0.86f;
        oval.set(cx - rx, cy - ry, cx + rx, cy + ry);

        float unit = Math.max(width, height) * 0.01f;
        ringGlow.setStrokeWidth(unit * 7.5f);
        canvas.drawArc(oval, 182f, 176f, false, ringGlow);
        ringBand.setStrokeWidth(unit * 3.1f);
        canvas.drawArc(oval, 182f, 176f, false, ringBand);
        ringInner.setStrokeWidth(unit * 0.16f);
        canvas.drawArc(oval, 183f, 174f, false, ringInner);
        ringOuter.setStrokeWidth(unit * 0.12f);
        oval.inset(unit * 1.55f, unit * 1.55f);
        canvas.drawArc(oval, 183f, 174f, false, ringOuter);
        oval.inset(-unit * 1.55f, -unit * 1.55f);
    }

    /** a sensor sweep, sliding down the pane */
    private void drawSweep(Canvas canvas, float width, float height, float time) {
        float band = height * 0.22f;
        float y = ((time * 0.045f) % 1.4f) * (height + band) - band;
        if (sweep == null)
            sweep = new Paint(Paint.ANTI_ALIAS_FLAG);
        sweep.setShader(new LinearGradient(0, y, 0, y + band,
            new int[] { 0x00000000, 0x147EE7FF, 0x267EE7FF, 0x147EE7FF, 0x00000000 },
            new float[] { 0f, 0.3f, 0.5f, 0.7f, 1f }, Shader.TileMode.CLAMP));
        canvas.drawRect(0, y, width, y + band, sweep);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        running = true;
        start = System.nanoTime();
        postInvalidateOnAnimation();
    }

    @Override
    protected void onDetachedFromWindow() {
        running = false;
        super.onDetachedFromWindow();
    }
}
