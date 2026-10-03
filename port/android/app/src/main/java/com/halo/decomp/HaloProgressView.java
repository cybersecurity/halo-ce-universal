package com.halo.decomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.util.AttributeSet;
import android.view.View;

/**
 * A segmented transfer bar: an array of cells that light up as the archive
 * is read, the way a UNSC console counts a data stream.
 */
public class HaloProgressView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int segments = 56;
    private float fraction;
    private boolean indeterminate;
    private long start;

    public HaloProgressView(Context context) {
        super(context);
        start = System.nanoTime();
    }

    public HaloProgressView(Context context, AttributeSet attributes) {
        super(context, attributes);
        start = System.nanoTime();
    }

    public void setProgress(float value) {
        fraction = Math.max(0f, Math.min(1f, value));
        indeterminate = false;
        invalidate();
    }

    public void setIndeterminate(boolean value) {
        indeterminate = value;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        float width = getWidth();
        float height = getHeight();
        if (width <= 0 || height <= 0)
            return;

        paint.setColor(0x3A0B2029);
        canvas.drawRect(0, 0, width, height, paint);

        float gap = Math.max(2f, width * 0.004f);
        float cell = (width - gap * (segments - 1)) / segments;
        int lit = (int) (fraction * segments + 0.001f);
        float time = (System.nanoTime() - start) / 1_000_000_000f;
        int head = indeterminate ? ((int) (time * 26f) % (segments + 12)) - 12 : lit - 1;

        for (int i = 0; i < segments; i++) {
            float left = i * (cell + gap);
            boolean on;
            if (indeterminate) {
                on = i >= head && i < head + 10;
            } else {
                on = i < lit;
            }
            if (on) {
                float distance = (head - i) / 10f;
                int color = blend(HaloUi.CYAN_DIM, HaloUi.CYAN, Math.max(0f, Math.min(1f, 1f - distance)));
                paint.setColor(color);
            } else {
                paint.setColor(0x227EE7FF);
            }
            canvas.drawRect(left, 0, left + cell, height, paint);
        }

        if (indeterminate && isShown())
            postInvalidateOnAnimation();
    }

    private static int blend(int from, int to, float t) {
        int r = (int) (((from >> 16) & 0xFF) + (((to >> 16) & 0xFF) - ((from >> 16) & 0xFF)) * t);
        int g = (int) (((from >> 8) & 0xFF) + (((to >> 8) & 0xFF) - ((from >> 8) & 0xFF)) * t);
        int b = (int) ((from & 0xFF) + ((to & 0xFF) - (from & 0xFF)) * t);
        return 0xFF000000 | r << 16 | g << 8 | b;
    }
}
