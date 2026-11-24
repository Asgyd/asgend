#!/usr/bin/env bash
# Flutter-generated wrapper scripts are tooling files. Bootstrap them from the
# pinned Flutter SDK template without overwriting the committed Android config.
set -euo pipefail

if ! command -v flutter >/dev/null 2>&1; then
  echo "error: Flutter SDK is required to bootstrap Gradle wrapper files" >&2
  exit 127
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
seed="$(mktemp -d)"
cleanup() { rm -rf "$seed"; }
trap cleanup EXIT

flutter create --platforms=android --project-name asgend_client --org org.asgend "$seed/seed" >/dev/null
mkdir -p "$here/android/gradle/wrapper"
cp "$seed/seed/android/gradlew" "$here/android/gradlew"
cp "$seed/seed/android/gradlew.bat" "$here/android/gradlew.bat"
cp "$seed/seed/android/gradle/wrapper/gradle-wrapper.jar" "$here/android/gradle/wrapper/gradle-wrapper.jar"
chmod +x "$here/android/gradlew"

# settings.gradle resolves the Flutter Gradle plugin through the untracked
# local.properties; create it for clean checkouts (CI) without touching an existing one.
props="$here/android/local.properties"
if [ ! -f "$props" ]; then
  flutter_root="$(cd "$(dirname "$(readlink -f "$(command -v flutter)")")/.." && pwd)"
  printf 'flutter.sdk=%s\n' "$flutter_root" > "$props"
  sdk_dir="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
  if [ -n "$sdk_dir" ]; then printf 'sdk.dir=%s\n' "$sdk_dir" >> "$props"; fi
fi
echo "Gradle wrapper bootstrapped. The committed gradle-wrapper.properties pins Gradle 8.10.2."
