#!/usr/bin/env bash
# Copies a Windows working tree into a Linux build tree for the Android
# build (tools/wsl_build_android.sh):
#
#     bash tools/wsl_sync_tree.sh SOURCE MIRROR WORK
#
# A Windows checkout (core.autocrlf) has CRLF line endings, which break the
# build's text tools (musl's mkalltypes.sed, for one). So SOURCE is first
# mirrored as it is (MIRROR), and each file that the mirror gets new is
# copied on to WORK with LF line endings and the mirror's time. Unchanged
# files are not touched, so ninja rebuilds only what changed. --delete
# matters: the build globs *.c, so a file deleted on Windows must also go
# away here. WORK's build/ is not in SOURCE and stays.
#
# The mirror is updated before WORK is. If a run stops between the two
# (rsync fails because a file changed under it, a copy fails, Ctrl-C), the
# mirror already has files that WORK lacks, and rsync would not report
# them again. So a marker stays while a sync is under way; when the next
# run finds it, it rebuilds the mirror, and every file is compared again.
# The mirror's times are the source's, so the files that did not change
# get the same time in WORK again, and ninja does not rebuild them.
set -euo pipefail

SOURCE="$1"
MIRROR="$2"
WORK="$3"
PENDING="$MIRROR.pending"

# what the sync leaves alone (not source, or local data that may be large)
EXCLUDES=(
	--exclude=/build/ --exclude=/dist/ --exclude=/assets/ --exclude=/original/ --exclude=/xbox/ --exclude=/.git/
	--exclude=/port/android/app/build/ --exclude=/port/android/build/ --exclude=/port/android/.gradle/
	--exclude=/port/android/local.properties
	--exclude=/build.ninja --exclude='/.ninja_*'
)

RECOVER=0
if [ -e "$PENDING" ]; then
	echo "the last sync did not finish: copying the whole tree again"
	rm -rf "$MIRROR"
	RECOVER=1
fi
mkdir -p "$MIRROR" "$WORK"
touch "$PENDING"
changes=$(mktemp)
trap 'rm -f "$changes"' EXIT
rsync -a --delete --out-format='%i %n' "${EXCLUDES[@]}" "$SOURCE/" "$MIRROR/" > "$changes"
if [ "$RECOVER" = 1 ]; then
	# the deletion that the failed run made in the mirror is not reported
	# again, so remove from WORK what the mirror no longer has
	rsync -r --delete --existing --ignore-existing "${EXCLUDES[@]}" "$MIRROR/" "$WORK/"
fi

# %i is 11 characters, then a space, then the path
copied=0
while IFS= read -r line; do
	item="${line:0:11}"
	path="${line:12}"
	case "$item" in
	'*deleting'*)
		rm -rf "${WORK:?}/$path"
		;;
	cd*)
		mkdir -p "$WORK/$path"
		;;
	'>f'*)
		mkdir -p "$(dirname "$WORK/$path")"
		if grep -Iq . "$MIRROR/$path" && grep -q $'\r' "$MIRROR/$path"; then
			sed 's/\r$//' "$MIRROR/$path" > "$WORK/$path"
		else
			cp "$MIRROR/$path" "$WORK/$path"
		fi
		chmod --reference="$MIRROR/$path" "$WORK/$path"
		touch -r "$MIRROR/$path" "$WORK/$path"
		copied=$((copied + 1))
		;;
	esac
done < "$changes"
rm -f "$PENDING"
echo "copied $copied changed files to $WORK"
