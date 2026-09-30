#!/bin/sh
# Signs build/macos/Halo.app with a Developer ID for distribution, with the
# hardened runtime, and notarizes it if a notarytool keychain profile is
# given. Run from the repository's root after ninja macos_app.
#
#   port/macos/sign_app.sh                 sign (the first Developer ID Application identity)
#   SIGN_IDENTITY=<sha1 or name> port/macos/sign_app.sh
#   NOTARY_PROFILE=<profile> port/macos/sign_app.sh    sign, notarize and staple
#   APP=build/macos-release/Halo.app port/macos/sign_app.sh   another bundle (the release build)
#
# A notarytool profile is made once with
#   xcrun notarytool store-credentials <profile> --apple-id <id> --team-id <team>
# (it asks for an app-specific password).
set -e
app=${APP:-build/macos/Halo.app}
out=$(dirname "$app")
identity=${SIGN_IDENTITY:-$(security find-identity -v -p codesigning | awk '/Developer ID Application/ {print $2; exit}')}
if [ -z "$identity" ]; then
	echo "no Developer ID Application identity in the keychain" >&2
	exit 1
fi
for library in "$app"/Contents/MacOS/*.dylib; do
	codesign --force --timestamp --options runtime --sign "$identity" "$library"
done
codesign --force --timestamp --options runtime --entitlements port/macos/Halo.entitlements \
	--sign "$identity" "$app"
codesign --verify --deep --strict --verbose=1 "$app"
if [ -n "$NOTARY_PROFILE" ]; then
	zip="$out/Halo.zip"
	rm -f "$zip"
	ditto -c -k --keepParent "$app" "$zip"
	xcrun notarytool submit "$zip" --keychain-profile "$NOTARY_PROFILE" --wait
	xcrun stapler staple "$app"
	spctl --assess --type execute --verbose "$app"
	rm -f "$zip"
	ditto -c -k --keepParent "$app" "$out/Halo-macos-arm64.zip"
	echo "notarized: $out/Halo-macos-arm64.zip"
else
	echo "signed (not notarized: set NOTARY_PROFILE to notarize)"
fi
