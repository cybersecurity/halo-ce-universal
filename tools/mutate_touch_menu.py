"""Mutation test of the menus' touch gestures (port/linux/src/touch_menu.c).

Each mutant is a copy of touch_menu.c with ONE textual change, built with
touch_menu_test.c exactly as tools/test_touch_menu.py builds it. A mutant is
KILLED when the build or the test run fails, SURVIVED when the tests pass.
The tracked source is never touched. Exits non-zero if a mutant survives.

    python3 tools/mutate_touch_menu.py               # the mutants
    python3 tools/mutate_touch_menu.py --equivalent  # also prove the equivalents survive
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port" / "linux" / "src" / "touch_menu.c"
HEADER = ROOT / "port" / "linux" / "src" / "touch_menu.h"
TEST = ROOT / "port" / "linux" / "tests" / "touch_menu_test.c"

GUARD = "if (!menu->finger_down || finger != menu->finger)\n\t\treturn;\n"
SLOP_X = "(dx > 0.0f ? menu->settings.slop : -menu->settings.slop)"
SLOP_Y = "(dy > 0.0f ? menu->settings.slop : -menu->settings.slop)"
CLICK_X = "menu->output.x = menu->output.click_x = menu->down_x;"
CLICK_Y = "menu->output.y = menu->output.click_y = menu->down_y;"

# (name, text found exactly once in touch_menu.c, replacement)
MUTANTS = [
    # init / reset
    ("init: settings not stored", "menu->settings = *settings;", "(void)settings;"),
    ("init: no memset", "memset(menu, 0, sizeof(*menu));", ""),
    ("reset: settings lost", "struct touch_menu_settings settings = menu->settings;",
     "struct touch_menu_settings settings = {0};"),
    # down
    ("down: second finger accepted", "if (menu->finger_down)\n\t\treturn;\n\t/* no state",
     "if (0)\n\t\treturn;\n\t/* no state"),
    ("down: finger_down not set", "menu->finger_down = 1;", ""),
    ("down: finger id not stored", "menu->finger = finger;", "(void)finger;"),
    ("down: down_x not stored", "menu->down_x = menu->last_x = x;", "menu->last_x = x;"),
    ("down: down_y not stored", "menu->down_y = menu->last_y = y;", "menu->last_y = y;"),
    ("down: remainder kept", "\tmenu->remainder = 0.0f;\n\t/* a finger", "\t/* a finger"),
    ("down: pending steps kept", "menu->pending_steps = 0;\n}\n\nvoid touch_menu_move",
     "}\n\nvoid touch_menu_move"),
    ("down: downs++ -> = 1", "menu->output.downs++;", "menu->output.downs = 1;"),
    ("down: downs not counted", "menu->output.downs++;", ""),
    ("down: downs++ -> --", "menu->output.downs++;", "menu->output.downs--;"),
    ("down: down_x not reported", "menu->output.down_x = x;", ""),
    ("down: down_y not reported", "menu->output.down_y = y;", ""),
    ("down: reported x is y", "menu->output.down_x = x;", "menu->output.down_x = y;"),
    ("down: reported y is x", "menu->output.down_y = y;", "menu->output.down_y = x;"),
    ("cancel: downs kept", "\tmenu->pending_steps = 0;\n\tmenu->output.downs = 0;\n}\n\nvoid touch_menu_read",
     "\tmenu->pending_steps = 0;\n}\n\nvoid touch_menu_read"),
    ("read: downs kept", "\tmenu->output.clicks = 0;\n\tmenu->output.downs = 0;", "\tmenu->output.clicks = 0;"),
    # move guards
    ("move: guard || -> &&", GUARD + "\tif (!menu->scrolling)",
     GUARD.replace("||", "&&") + "\tif (!menu->scrolling)"),
    ("move: finger id ignored", GUARD + "\tif (!menu->scrolling)",
     "if (!menu->finger_down)\n\t\treturn;\n\t(void)finger;\n\tif (!menu->scrolling)"),
    ("move: finger_down ignored", GUARD + "\tif (!menu->scrolling)",
     "if (finger != menu->finger)\n\t\treturn;\n\tif (!menu->scrolling)"),
    ("move: != -> ==", GUARD + "\tif (!menu->scrolling)",
     GUARD.replace("!=", "==") + "\tif (!menu->scrolling)"),
    ("move: slop test <= -> <", "dy * dy <= menu->settings.slop", "dy * dy < menu->settings.slop"),
    ("move: slop test <= -> >", "dy * dy <= menu->settings.slop", "dy * dy > menu->settings.slop"),
    ("move: slop not squared", "<= menu->settings.slop * menu->settings.slop", "<= menu->settings.slop"),
    ("move: distance dx*dx - dy*dy", "dx * dx + dy * dy", "dx * dx - dy * dy"),
    ("move: distance ignores dy", "dx * dx + dy * dy", "dx * dx"),
    ("move: distance ignores dx", "dx * dx + dy * dy", "dy * dy"),
    ("move: dx sign", "float dx = x - menu->down_x", "float dx = menu->down_x - x"),
    ("move: dy sign", "dy = y - menu->down_y", "dy = menu->down_y - y"),
    ("move: scrolling not set", "menu->scrolling = 1;\n\t\tmenu->axis", "menu->axis"),
    # axis choice
    ("axis: > -> >= (tie horizontal)", "fabsf(dx) > fabsf(dy)", "fabsf(dx) >= fabsf(dy)"),
    ("axis: > -> <", "fabsf(dx) > fabsf(dy)", "fabsf(dx) < fabsf(dy)"),
    ("axis: always vertical", "menu->axis = fabsf(dx) > fabsf(dy);", "menu->axis = 0;"),
    ("axis: always horizontal", "menu->axis = fabsf(dx) > fabsf(dy);", "menu->axis = 1;"),
    ("axis: no fabsf on dx", "fabsf(dx) > fabsf(dy)", "dx > fabsf(dy)"),
    ("axis: no fabsf on dy", "fabsf(dx) > fabsf(dy)", "fabsf(dx) > dy"),
    ("axis: edge branch inverted", "if (menu->axis)\n\t\t\tmenu->last_x", "if (!menu->axis)\n\t\t\tmenu->last_x"),
    # slop-edge reset of the start point
    ("edge: x not reset", "\t\t\tmenu->last_x = menu->down_x + " + SLOP_X + ";\n",
     "\t\t\tmenu->last_x = menu->down_x;\n"),
    ("edge: y not reset", "\t\t\tmenu->last_y = menu->down_y + " + SLOP_Y + ";\n",
     "\t\t\tmenu->last_y = menu->down_y;\n"),
    ("edge: x sign of slop swapped", SLOP_X, "(dx > 0.0f ? -menu->settings.slop : menu->settings.slop)"),
    ("edge: y sign of slop swapped", SLOP_Y, "(dy > 0.0f ? -menu->settings.slop : menu->settings.slop)"),
    ("edge: x + -> -", "menu->down_x + (dx", "menu->down_x - (dx"),
    ("edge: y + -> -", "menu->down_y + (dy", "menu->down_y - (dy"),
    ("edge: x test dx > -> <", "(dx > 0.0f ?", "(dx < 0.0f ?"),
    ("edge: y test dy > -> <", "(dy > 0.0f ?", "(dy < 0.0f ?"),
    ("edge: x from down_y", "menu->last_x = menu->down_x + (", "menu->last_x = menu->down_y + ("),
    ("edge: x sign from dy", "menu->last_x = menu->down_x + (dx", "menu->last_x = menu->down_x + (dy"),
    ("edge: y sign from dx", "menu->last_y = menu->down_y + (dy", "menu->last_y = menu->down_y + (dx"),
    ("edge: half a slop", "(dx > 0.0f ? menu->settings.slop :", "(dx > 0.0f ? menu->settings.slop / 2.0f :"),
    # along, remainder, steps
    ("along: axis swapped", "along = menu->axis ? x - menu->last_x : y - menu->last_y;",
     "along = menu->axis ? y - menu->last_y : x - menu->last_x;"),
    ("along: x sign", "menu->axis ? x - menu->last_x", "menu->axis ? menu->last_x - x"),
    ("along: y sign", ": y - menu->last_y;", ": menu->last_y - y;"),
    ("along: x against down", "menu->axis ? x - menu->last_x", "menu->axis ? x - menu->down_x"),
    ("along: y against down", ": y - menu->last_y;", ": y - menu->down_y;"),
    ("last_x not updated", "menu->last_x = x;\n\tmenu->last_y = y;", "menu->last_y = y;"),
    ("last_y not updated", "menu->last_x = x;\n\tmenu->last_y = y;", "menu->last_x = x;"),
    ("remainder = instead of +=", "menu->remainder += along;", "menu->remainder = along;"),
    ("remainder -= along", "menu->remainder += along;", "menu->remainder -= along;"),
    ("remainder not added", "menu->remainder += along;", "(void)along;"),
    ("back loop: >= -> >", "menu->remainder >= menu->settings.step", "menu->remainder > menu->settings.step"),
    ("back loop: >= -> <=", "menu->remainder >= menu->settings.step", "menu->remainder <= menu->settings.step"),
    ("back loop: step doubled", "menu->remainder >= menu->settings.step", "menu->remainder >= menu->settings.step * 2.0f"),
    ("back loop: ++ -> --", "menu->pending_steps++;", "menu->pending_steps--;"),
    ("back loop: -= -> +=", "menu->remainder -= menu->settings.step;", "menu->remainder += menu->settings.step;"),
    ("back loop: subtraction dropped",
     "\t\tmenu->pending_steps++;\n\t\tmenu->remainder -= menu->settings.step;\n\t}\n\twhile",
     "\t\tmenu->pending_steps++;\n\t\tmenu->remainder = 0.0f;\n\t}\n\twhile"),
    ("back loop: while -> if", "while (menu->remainder >= menu->settings.step)", "if (menu->remainder >= menu->settings.step)"),
    ("forward loop: <= -> <", "menu->remainder <= -menu->settings.step", "menu->remainder < -menu->settings.step"),
    ("forward loop: <= -> >=", "menu->remainder <= -menu->settings.step", "menu->remainder >= -menu->settings.step"),
    ("forward loop: no minus", "menu->remainder <= -menu->settings.step", "menu->remainder <= menu->settings.step"),
    ("forward loop: -- -> ++", "menu->pending_steps--;", "menu->pending_steps++;"),
    ("forward loop: += -> -=", "menu->remainder += menu->settings.step;\n\t}\n}", "menu->remainder -= menu->settings.step;\n\t}\n}"),
    ("forward loop: subtraction dropped",
     "\t\tmenu->pending_steps--;\n\t\tmenu->remainder += menu->settings.step;",
     "\t\tmenu->pending_steps--;\n\t\tmenu->remainder = 0.0f;"),
    ("forward loop: while -> if", "while (menu->remainder <= -menu->settings.step)", "if (menu->remainder <= -menu->settings.step)"),
    # up
    ("up: guard || -> &&", GUARD + "\ttouch_menu_move", GUARD.replace("||", "&&") + "\ttouch_menu_move"),
    ("up: finger id ignored", GUARD + "\ttouch_menu_move", "if (!menu->finger_down)\n\t\treturn;\n\ttouch_menu_move"),
    ("up: finger_down ignored", GUARD + "\ttouch_menu_move", "if (finger != menu->finger)\n\t\treturn;\n\ttouch_menu_move"),
    ("up: != -> ==", GUARD + "\ttouch_menu_move", GUARD.replace("!=", "==") + "\ttouch_menu_move"),
    ("up: final move dropped", "\ttouch_menu_move(menu, finger, x, y);\n\tif (!menu->scrolling &&", "\t(void)x;\n\t(void)y;\n\tif (!menu->scrolling &&"),
    ("up: tap test inverted", "\tif (!menu->scrolling && !menu->void_tap)\n\t{\n\t\tmenu->output.moved", "\tif (menu->scrolling)\n\t{\n\t\tmenu->output.moved"),
    ("up: tap always", "\tif (!menu->scrolling && !menu->void_tap)\n\t{\n\t\tmenu->output.moved", "\tif (1)\n\t{\n\t\tmenu->output.moved"),
    ("up: moved not set", "menu->output.moved = 1;", ""),
    ("up: moved = 0", "menu->output.moved = 1;", "menu->output.moved = 0;"),
    ("up: x from the lift", CLICK_X, CLICK_X.replace("menu->down_x", "x")),
    ("up: y from the lift", CLICK_Y, CLICK_Y.replace("menu->down_y", "y")),
    ("up: x from down_y", CLICK_X, CLICK_X.replace("down_x", "down_y")),
    ("up: y from down_x", CLICK_Y, CLICK_Y.replace("down_y", "down_x")),
    ("up: pointer x not set", CLICK_X, "menu->output.click_x = menu->down_x;"),
    ("up: click x not set", CLICK_X, "menu->output.x = menu->down_x;"),
    ("up: pointer y not set", CLICK_Y, "menu->output.click_y = menu->down_y;"),
    ("up: click y not set", CLICK_Y, "menu->output.y = menu->down_y;"),
    ("up: clicks++ -> = 1", "menu->output.clicks++;", "menu->output.clicks = 1;"),
    ("up: clicks not counted", "menu->output.clicks++;", ""),
    ("up: clicks++ -> --", "menu->output.clicks++;", "menu->output.clicks--;"),
    ("up: finger stays down", "\tmenu->finger_down = 0;\n\tmenu->scrolling = 0;\n\tmenu->void_tap = 0;\n}\n\nvoid touch_menu_cancel",
     "\tmenu->scrolling = 0;\n\tmenu->void_tap = 0;\n}\n\nvoid touch_menu_cancel"),
    # cancel
    ("cancel: finger stays down", "void touch_menu_cancel(struct touch_menu *menu)\n{\n\tmenu->finger_down = 0;\n",
     "void touch_menu_cancel(struct touch_menu *menu)\n{\n"),
    ("cancel: pending steps kept", "\tmenu->remainder = 0.0f;\n\tmenu->pending_steps = 0;\n\tmenu->output.downs = 0;\n}",
     "\tmenu->remainder = 0.0f;\n\tmenu->output.downs = 0;\n}"),
    ("cancel: unread tap dropped too", "menu->pending_steps = 0;\n\tmenu->output.downs = 0;\n}\n\nvoid touch_menu_read",
     "menu->pending_steps = 0;\n\tmenu->output.downs = 0;\n\tmenu->output.clicks = 0;\n\tmenu->output.moved = 0;\n}\n\nvoid touch_menu_read"),
    # gesture zones
    ("zone: left < -> <=", "return x < s->edge_left ||", "return x <= s->edge_left ||"),
    ("zone: left check removed", "return x < s->edge_left ||", "return 0 ||"),
    ("zone: left uses the right inset", "return x < s->edge_left ||", "return x < s->edge_right ||"),
    ("zone: right >= -> >", "x >= s->width - s->edge_right", "x > s->width - s->edge_right"),
    ("zone: right check removed", " || (s->edge_right > 0.0f && s->width > 0.0f && x >= s->width - s->edge_right)", ""),
    ("zone: right without the width guard", "s->width > 0.0f && x >= s->width - s->edge_right)",
     "x >= s->width - s->edge_right)"),
    ("zone: right without the inset guard", "(s->edge_right > 0.0f && s->width", "(s->width"),
    ("zone: right uses the left inset", "x >= s->width - s->edge_right", "x >= s->width - s->edge_left"),
    ("zone: top < -> <=", "return y < s->edge_top ||", "return y <= s->edge_top ||"),
    ("zone: top check removed", "return y < s->edge_top ||", "return 0 ||"),
    ("zone: top uses the bottom inset", "return y < s->edge_top ||", "return y < s->edge_bottom ||"),
    ("zone: bottom >= -> >", "y >= s->height - s->edge_bottom", "y > s->height - s->edge_bottom"),
    ("zone: bottom check removed", " || (s->edge_bottom > 0.0f && s->height > 0.0f && y >= s->height - s->edge_bottom)", ""),
    ("zone: bottom without the height guard", "s->height > 0.0f && y >= s->height - s->edge_bottom)",
     "y >= s->height - s->edge_bottom)"),
    ("zone: bottom without the inset guard", "(s->edge_bottom > 0.0f && s->height", "(s->height"),
    ("zone: bottom uses the top inset", "y >= s->height - s->edge_bottom", "y >= s->height - s->edge_top"),
    ("down: side zone finger accepted", "\tif (in_side_zone(menu, x))\n\t\treturn;\n",
     "\tif (0 && in_side_zone(menu, x))\n\t\treturn;\n"),
    ("down: side zone finger blocks the next",
     "\tif (in_side_zone(menu, x))\n\t\treturn;\n",
     "\tif (in_side_zone(menu, x))\n\t{\n\t\tmenu->finger_down = 1;\n\t\tmenu->finger = finger;\n\t\treturn;\n\t}\n"),
    ("down: side zone finger counts as a down",
     "\tif (in_side_zone(menu, x))\n\t\treturn;\n",
     "\tif (in_side_zone(menu, x))\n\t{\n\t\tmenu->output.downs++;\n\t\treturn;\n\t}\n"),
    ("down: side zone finger drops the pending steps",
     "\tif (in_side_zone(menu, x))\n\t\treturn;\n",
     "\tif (in_side_zone(menu, x))\n\t{\n\t\tmenu->pending_steps = 0;\n\t\treturn;\n\t}\n"),
    ("down: no_scroll never set", "menu->no_scroll = in_top_or_bottom_zone(menu, y);", "menu->no_scroll = 0 && in_top_or_bottom_zone(menu, y);"),
    ("down: no_scroll always set", "menu->no_scroll = in_top_or_bottom_zone(menu, y);", "menu->no_scroll = 1 || in_top_or_bottom_zone(menu, y);"),
    ("down: no_scroll from x", "menu->no_scroll = in_top_or_bottom_zone(menu, y);", "menu->no_scroll = in_top_or_bottom_zone(menu, x);"),
    ("move: no-scroll finger scrolls", "\t\t\tmenu->void_tap = 1;\n\t\t\treturn;", "\t\t\tmenu->void_tap = 1;"),
    ("move: zone judged again at the move", "\t\tif (menu->no_scroll)\n",
     "\t\tif (in_top_or_bottom_zone(menu, menu->down_y))\n"),
    ("move: no_scroll test removed", "\t\tif (menu->no_scroll)\n", "\t\tif (0)\n"),
    ("move: no-scroll finger keeps its tap", "\t\t\tmenu->void_tap = 1;\n", ""),
    ("up: no-scroll finger clicks", "if (!menu->scrolling && !menu->void_tap)", "if (!menu->scrolling)"),
    ("up: void test inverted", "if (!menu->scrolling && !menu->void_tap)", "if (!menu->scrolling && menu->void_tap)"),
    # read
    ("read: cap > -> <", "if (steps > limit)", "if (steps < limit)"),
    ("read: upper cap dropped", "\tif (steps > limit)\n\t\tsteps = limit;\n", ""),
    ("read: upper cap wrong value", "\t\tsteps = limit;", "\t\tsteps = limit + 1;"),
    ("read: upper cap sign", "\t\tsteps = limit;", "\t\tsteps = -limit;"),
    ("read: lower cap < -> >", "if (steps < -limit)", "if (steps > -limit)"),
    ("read: lower cap dropped", "\tif (steps < -limit)\n\t\tsteps = -limit;\n", ""),
    ("read: lower cap wrong value", "\t\tsteps = -limit;", "\t\tsteps = -limit - 1;"),
    ("read: lower cap sign", "\t\tsteps = -limit;", "\t\tsteps = limit;"),
    ("read: limit is 1", "int limit = menu->settings.steps_per_read;", "int limit = 1;"),
    ("read: limit + 1", "int limit = menu->settings.steps_per_read;", "int limit = menu->settings.steps_per_read + 1;"),
    ("read: limit - 1", "int limit = menu->settings.steps_per_read;", "int limit = menu->settings.steps_per_read - 1;"),
    ("read: steps from nothing", "int steps = menu->pending_steps;", "int steps = 0;"),
    ("read: output not copied", "\t*output = menu->output;\n", ""),
    ("read: wheel steps not set", "output->wheel_steps = steps;", ""),
    ("read: wheel steps negated", "output->wheel_steps = steps;", "output->wheel_steps = -steps;"),
    ("read: pending not consumed", "menu->pending_steps -= steps;", ""),
    ("read: pending -= -> +=", "menu->pending_steps -= steps;", "menu->pending_steps += steps;"),
    ("read: pending cleared", "menu->pending_steps -= steps;", "menu->pending_steps = 0;"),
    ("read: moved kept", "menu->output.moved = 0;\n\tmenu->output.clicks = 0;", "menu->output.clicks = 0;"),
    ("read: clicks kept", "menu->output.moved = 0;\n\tmenu->output.clicks = 0;", "menu->output.moved = 0;"),
]

# Mutants that cannot change behaviour, and why. Run only with --equivalent,
# to prove that each one really survives.
EQUIVALENT = [
    ("down: last_x/last_y not initialised",
     "menu->down_x = menu->last_x = x;\n\tmenu->down_y = menu->last_y = y;",
     "menu->down_x = x;\n\tmenu->down_y = y;",
     "the first move past the slop rewrites last on the scroll axis before it is read, "
     "and the other axis's last is never read; a move inside the slop reads neither"),
    ("down: scrolling not cleared",
     "menu->scrolling = 0;\n\tmenu->no_scroll",
     "menu->no_scroll",
     "scrolling is already 0 whenever finger_down is 0: init, up and cancel clear it, and down "
     "runs only when finger_down is 0"),
    ("up: scrolling not cleared",
     "\tmenu->finger_down = 0;\n\tmenu->scrolling = 0;\n\tmenu->void_tap = 0;\n}\n\nvoid touch_menu_cancel",
     "\tmenu->finger_down = 0;\n\tmenu->void_tap = 0;\n}\n\nvoid touch_menu_cancel",
     "scrolling is read only while finger_down is set, and down clears it before the next finger"),
    ("up: void_tap not cleared",
     "\tmenu->scrolling = 0;\n\tmenu->void_tap = 0;\n}\n\nvoid touch_menu_cancel",
     "\tmenu->scrolling = 0;\n}\n\nvoid touch_menu_cancel",
     "void_tap is read only while finger_down is set (up), and down clears it before the next finger"),
    ("cancel: scrolling not cleared",
     "menu->finger_down = 0;\n\tmenu->scrolling = 0;\n\tmenu->void_tap = 0;\n\tmenu->remainder = 0.0f;",
     "menu->finger_down = 0;\n\tmenu->void_tap = 0;\n\tmenu->remainder = 0.0f;",
     "same as up: scrolling is read only while finger_down is set, and down clears it"),
    ("cancel: void_tap not cleared",
     "menu->scrolling = 0;\n\tmenu->void_tap = 0;\n\tmenu->remainder = 0.0f;",
     "menu->scrolling = 0;\n\tmenu->remainder = 0.0f;",
     "same as up: void_tap is read only while finger_down is set, and down clears it"),
    ("down: void_tap not cleared",
     "menu->void_tap = 0;\n\tmenu->remainder = 0.0f;\n\t/* a finger",
     "menu->remainder = 0.0f;\n\t/* a finger",
     "up and cancel already clear void_tap, and it is set only while a finger is down"),
    ("cancel: remainder not cleared",
     "menu->void_tap = 0;\n\tmenu->remainder = 0.0f;\n\tmenu->pending_steps = 0;\n\tmenu->output.downs = 0;\n}",
     "menu->void_tap = 0;\n\tmenu->pending_steps = 0;\n\tmenu->output.downs = 0;\n}",
     "remainder is read only by move, which needs finger_down, and down zeroes it"),
    ("edge: dx >= 0 for the sign of the x edge", "(dx > 0.0f ?", "(dx >= 0.0f ?",
     "the x edge is computed only when |dx| > |dy| >= 0, so dx is never 0 there"),
    ("edge: dy >= 0 for the sign of the y edge", "(dy > 0.0f ?", "(dy >= 0.0f ?",
     "on the vertical axis |dy| >= |dx| and dx*dx+dy*dy > slop*slop > 0, so dy is never 0"),
    ("move: dx measured from last_x", "float dx = x - menu->down_x", "float dx = x - menu->last_x",
     "dx is computed only before scrolling starts, and until then last_x still equals down_x"),
    ("move: dy measured from last_y", "dy = y - menu->down_y", "dy = y - menu->last_y",
     "same as dx: last_y equals down_y until scrolling starts"),
    ("up: tap x taken from last_x", "menu->output.x = menu->output.click_x = menu->down_x;",
     "menu->output.x = menu->output.click_x = menu->last_x;",
     "a tap is a gesture that never scrolled, so last_x was never moved off down_x"),
    ("up: tap y taken from last_y", "menu->output.y = menu->output.click_y = menu->down_y;",
     "menu->output.y = menu->output.click_y = menu->last_y;",
     "a tap is a gesture that never scrolled, so last_y was never moved off down_y"),
    ("read: upper cap >= (assigns limit to limit)", "if (steps > limit)", "if (steps >= limit)",
     "at steps == limit the assignment changes nothing"),
    ("read: lower cap <= (assigns -limit to -limit)", "if (steps < -limit)", "if (steps <= -limit)",
     "at steps == -limit the assignment changes nothing"),
]


def compiler():
    """Find the build machine's C compiler.

    Returns the path of clang, cc or gcc, whichever exists first; exits if none.
    """
    for name in ("clang", "cc", "gcc"):
        path = shutil.which(name)
        if path:
            return path
    sys.exit("no C compiler")


def run_mutant(cc, work, text):
    """Build touch_menu_test.c against one mutant and run it.

    cc: the compiler path. work: a temporary directory that holds the mutant
    and a copy of touch_menu.h. text: the mutant's source.
    Returns ("KILLED", reason) when the tests fail or hang, ("SURVIVED", "")
    when they pass. A build failure exits the script: it is a broken mutation,
    not a kill.
    """
    source = work / "touch_menu.c"
    source.write_text(text, newline="\n")
    binary = work / "touch_menu_test"
    build = subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", str(TEST), str(source),
         "-lm", "-o", str(binary)],
        capture_output=True, text=True)
    if build.returncode != 0:
        # a mutant that does not compile proves nothing about the tests
        sys.exit(f"script error: the mutant does not build:\n{build.stderr}")
    try:
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    except subprocess.TimeoutExpired:
        return "KILLED", "timeout"
    if result.returncode == 0:
        return "SURVIVED", ""
    return "KILLED", "test: " + (result.stdout.splitlines() or ["?"])[0]


def main():
    """Run every mutant, and with --equivalent the equivalent ones too.

    Returns the exit code: 1 if a mutant survived or an equivalent one was
    killed, else 0.
    """
    cc = compiler()
    original = SOURCE.read_text().replace("\r\n", "\n")
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        # the mutant includes "touch_menu.h" from its own directory
        shutil.copy(HEADER, work / "touch_menu.h")
        killed = 0
        for name, old, new in MUTANTS:
            if original.count(old) != 1:
                sys.exit(f"mutation '{name}': {original.count(old)} matches, need exactly 1")
            verdict, how = run_mutant(cc, work, original.replace(old, new))
            print(f"{verdict:8} {name} {how}".rstrip())
            killed += verdict == "KILLED"
        print(f"killed {killed}/{len(MUTANTS)}")
        failed = killed != len(MUTANTS)
        if "--equivalent" in sys.argv:
            for name, old, new, why in EQUIVALENT:
                if original.count(old) != 1:
                    sys.exit(f"equivalent '{name}': {original.count(old)} matches, need exactly 1")
                verdict, _ = run_mutant(cc, work, original.replace(old, new))
                print(f"equivalent {'ok' if verdict == 'SURVIVED' else 'KILLED'}: {name} ({why})")
                failed = failed or verdict != "SURVIVED"
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
