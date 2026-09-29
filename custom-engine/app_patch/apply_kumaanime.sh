#!/usr/bin/env bash
set -euo pipefail

APP_DIR="${1:?usage: apply_kumaanime.sh <KumaAnime-App-directory>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PATCH="$HERE/0001-kumaanime-packed-release.patch"

[[ -f "$APP_DIR/android/app/build.gradle" ]] || {
  echo "KumaAnime Groovy build.gradle not found: $APP_DIR" >&2
  exit 2
}

check_dolby() {
  local name="$1" expected="$2" path="$APP_DIR/android/app/src/main/assets/dolby/$1"
  [[ -f "$path" ]] || return 0
  local actual
  actual="$(shasum -a 256 "$path" | cut -d' ' -f1)"
  [[ "$actual" == "$expected" ]] || {
    echo "refusing to remove changed Dolby source asset: $path" >&2
    exit 1
  }
}

check_dolby DolbySound.apk 7985f0cbc3909629b1e5afd744b2720f8c0ed52d796ceb3da2e9d26b889c55f8
check_dolby daxService.apk 510cba072e830baf0a39ddb585998cb81b8164b8ec4bc06c9a8e1d00f9f9b340
patch --dry-run -p1 -d "$APP_DIR" < "$PATCH"
patch -p1 -d "$APP_DIR" < "$PATCH"
chmod +x "$APP_DIR/tool/build_release_hidden_assets.sh"
for name in DolbySound.apk daxService.apk; do
  path="$APP_DIR/android/app/src/main/assets/dolby/$name"
  [[ ! -f "$path" ]] || rm -- "$path"
done
echo "KumaAnime packed-release integration applied; build with flutter build apk --release"
