package com.halo.decomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.TextView;

/**
 * The look of the interface: an ONI archive terminal. Dark void, holographic
 * cyan, the odd amber warning, condensed uppercase display type and a
 * monospaced readout, chamfered panels and panes.
 */
final class HaloUi {
    private HaloUi() {}

    static final int VOID = Color.rgb(1, 5, 9);
    static final int DEEP = Color.rgb(4, 15, 23);
    static final int CYAN = Color.rgb(126, 231, 255);
    static final int CYAN_DIM = Color.rgb(52, 132, 156);
    static final int CYAN_FAINT = Color.rgb(24, 62, 76);
    static final int AMBER = Color.rgb(240, 180, 70);
    static final int TEXT = Color.rgb(208, 235, 241);
    static final int TEXT_DIM = Color.rgb(126, 166, 176);
    static final int PANEL = 0xC407141C;
    static final int PANEL_EDGE = 0x8A2E8AA6;

    static final Typeface DISPLAY = Typeface.create("sans-serif-condensed", Typeface.NORMAL);
    static final Typeface DISPLAY_BOLD = Typeface.create("sans-serif-condensed", Typeface.BOLD);
    static final Typeface MONO = Typeface.create(Typeface.MONOSPACE, Typeface.NORMAL);

    static int dp(Context context, float value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
            context.getResources().getDisplayMetrics());
    }

    static TextView text(Context context, String value, int color, float sizeSp, Typeface typeface) {
        TextView view = new TextView(context);
        view.setText(value);
        view.setTextColor(color);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp);
        view.setTypeface(typeface);
        return view;
    }

    /** a condensed, letter-spaced heading, the way a UNSC terminal labels a pane */
    static TextView heading(Context context, String value, int color, float sizeSp) {
        TextView view = text(context, value, color, sizeSp, DISPLAY_BOLD);
        view.setAllCaps(true);
        view.setLetterSpacing(0.28f);
        view.setGravity(Gravity.START);
        return view;
    }

    static TextView readout(Context context, String value, int color, float sizeSp) {
        TextView view = text(context, value, color, sizeSp, MONO);
        view.setLineSpacing(dp(context, 3), 1f);
        return view;
    }

    /** a panel fill, cut at the corners and edged in faint holo-cyan */
    static Drawable panel(Context context) {
        Drawable fill = new Chamfer(PANEL, PANEL_EDGE, dp(context, 1), dp(context, 18));
        Drawable brackets = new Brackets(dp(context, 22), Math.max(1.5f, dp(context, 1.5f)), 0xBF7EE7FF);
        return new LayerDrawable(new Drawable[] { fill, brackets });
    }

    static void styleButton(Context context, Button button) {
        button.setAllCaps(false);
        button.setTypeface(DISPLAY_BOLD);
        button.setLetterSpacing(0.16f);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        button.setGravity(Gravity.CENTER);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setMinimumWidth(0);
        button.setPadding(dp(context, 22), dp(context, 13), dp(context, 22), dp(context, 13));
        button.setStateListAnimator(null);
        button.setTextColor(new android.content.res.ColorStateList(
            new int[][] { new int[] { android.R.attr.state_pressed }, new int[] {} },
            new int[] { VOID, CYAN }));
        button.setBackground(buttonBackground(context));
    }

    private static Drawable buttonBackground(Context context) {
        int cut = dp(context, 12);
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] { android.R.attr.state_pressed },
            new Chamfer(0xF07EE7FF, 0xFF7EE7FF, dp(context, 1), cut));
        states.addState(new int[] { -android.R.attr.state_enabled },
            new Chamfer(0x40101C22, 0x40527280, dp(context, 1), cut));
        states.addState(new int[] {},
            new Chamfer(0xE00A1A24, 0xC07EE7FF, dp(context, 1), cut));
        return states;
    }

    /** the filled variant for the one primary action on a screen */
    static Drawable primaryBackground(Context context) {
        int cut = dp(context, 14);
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] { android.R.attr.state_pressed },
            new Chamfer(0xFFFFFFFF, 0xFFFFFFFF, dp(context, 1), cut));
        states.addState(new int[] {},
            new Chamfer(0xFF7EE7FF, 0xFFFFFFFF, dp(context, 1), cut));
        return states;
    }

    /** a tappable candidate pane, brighter while pressed */
    static Drawable entryBackground(Context context) {
        int cut = dp(context, 12);
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] { android.R.attr.state_pressed },
            new Chamfer(0xE61B4656, 0xFF7EE7FF, dp(context, 1), cut));
        states.addState(new int[] {},
            new Chamfer(0x7A0A1A24, 0x667EE7FF, dp(context, 1), cut));
        return states;
    }

    /** corner brackets, so a pane reads as a framed instrument panel */
    static final class Brackets extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final float arm;
        private final float stroke;

        Brackets(float arm, float stroke, int color) {
            this.arm = arm;
            this.stroke = stroke;
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(stroke);
            paint.setColor(color);
        }

        @Override
        public void draw(Canvas canvas) {
            float width = getBounds().width();
            float height = getBounds().height();
            float a = Math.min(arm, Math.min(width, height) * 0.2f);
            float o = stroke / 2f;
            canvas.save();
            canvas.translate(getBounds().left, getBounds().top);
            canvas.drawLine(o, o, a, o, paint);
            canvas.drawLine(o, o, o, a, paint);
            canvas.drawLine(width - a, o, width - o, o, paint);
            canvas.drawLine(width - o, o, width - o, a, paint);
            canvas.drawLine(width - a, height - o, width - o, height - o, paint);
            canvas.drawLine(width - o, height - a, width - o, height - o, paint);
            canvas.drawLine(o, height - o, a, height - o, paint);
            canvas.drawLine(o, height - a, o, height - o, paint);
            canvas.restore();
        }

        @Override
        public void setAlpha(int alpha) {
            paint.setAlpha(alpha);
        }

        @Override
        public void setColorFilter(ColorFilter colorFilter) {
            paint.setColorFilter(colorFilter);
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }

    /** a hexagon-ish pane: a rectangle with its four corners cut off */
    static final class Chamfer extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        private final int fill;
        private final int stroke;
        private final float strokeWidth;
        private final float cut;

        Chamfer(int fill, int stroke, float strokeWidth, float cut) {
            this.fill = fill;
            this.stroke = stroke;
            this.strokeWidth = strokeWidth;
            this.cut = cut;
        }

        @Override
        public void draw(Canvas canvas) {
            float width = getBounds().width();
            float height = getBounds().height();
            float c = Math.min(cut, Math.min(width, height) / 2f);
            path.reset();
            path.moveTo(c, 0);
            path.lineTo(width - c, 0);
            path.lineTo(width, c);
            path.lineTo(width, height - c);
            path.lineTo(width - c, height);
            path.lineTo(c, height);
            path.lineTo(0, height - c);
            path.lineTo(0, c);
            path.close();
            canvas.save();
            canvas.translate(getBounds().left, getBounds().top);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(fill);
            canvas.drawPath(path, paint);
            if (strokeWidth > 0 && Color.alpha(stroke) != 0) {
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(strokeWidth);
                paint.setColor(stroke);
                canvas.drawPath(path, paint);
            }
            canvas.restore();
        }

        @Override
        public void setAlpha(int alpha) {
            paint.setAlpha(alpha);
        }

        @Override
        public void setColorFilter(ColorFilter colorFilter) {
            paint.setColorFilter(colorFilter);
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }
}
