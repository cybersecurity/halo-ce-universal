package com.halo.decomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.view.View;

/**
 * A small rotating holo-glyph, the little "working" tell of an ONI terminal.
 */
public class HaloGlyphView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF oval = new RectF();
    private long start;
    private boolean running;

    public HaloGlyphView(Context context) {
        super(context);
    }

    public HaloGlyphView(Context context, AttributeSet attributes) {
        super(context, attributes);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        float width = getWidth();
        float height = getHeight();
        float time = (System.nanoTime() - start) / 1_000_000_000f;
        float cx = width / 2f;
        float cy = height / 2f;
        float radius = Math.min(width, height) * 0.38f;

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(1f, radius * 0.09f));
        paint.setColor(0x557EE7FF);
        canvas.drawCircle(cx, cy, radius, paint);

        // two counter-rotating arcs
        paint.setStrokeWidth(Math.max(1f, radius * 0.16f));
        paint.setColor(HaloUi.CYAN);
        for (int i = 0; i < 2; i++) {
            float rotation = time * (i == 0 ? 150f : -230f);
            oval.set(cx - radius, cy - radius, cx + radius, cy + radius);
            canvas.drawArc(oval, rotation, i == 0 ? 96f : 58f, false, paint);
        }

        // an orbiting mote and a pulsing core
        float angle = time * 2.1f;
        float orbit = radius * 0.62f;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xFFDFF7FF);
        canvas.drawCircle(cx + (float) Math.cos(angle) * orbit, cy + (float) Math.sin(angle) * orbit,
            Math.max(1f, radius * 0.12f), paint);

        float pulse = 0.55f + 0.45f * (float) Math.sin(time * 3.4f);
        paint.setColor(0xFF7EE7FF);
        canvas.drawCircle(cx, cy, radius * 0.22f * pulse, paint);

        if (isShown())
            postInvalidateOnAnimation();
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
