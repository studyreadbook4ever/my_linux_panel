#!/usr/bin/env bash
set -euo pipefail

APP_NAME="myPanel"
INSTALL_PATH="/usr/local/bin/myPanel"
AUTOSTART_NAME="mypanel.desktop"
DEFAULT_WEATHER_PLACE="Seoul"
DEFAULT_WEATHER_URL="https://api.open-meteo.com/v1/forecast?latitude=37.5665&longitude=126.9780&current=temperature_2m,weather_code&timezone=auto"
REQUIRED_PACKAGES=(
  base-devel
  gtk3
  pkgconf
  curl
  exo
  xfce4-settings
  xfwm4
  xfconf
  clang
)

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

die() {
  printf 'Error: %s\n' "$*" >&2
  exit 1
}

info() {
  printf '\n==> %s\n' "$*"
}

prompt_choice() {
  local prompt="$1"
  local max_choice="$2"
  local choice

  while true; do
    printf '%s ' "$prompt" >&2
    IFS= read -r choice
    case "$choice" in
      ''|*[!0-9]*) printf 'Please enter a number from 1 to %s.\n' "$max_choice" >&2 ;;
      *)
        if (( choice >= 1 && choice <= max_choice )); then
          printf '%s\n' "$choice"
          return 0
        fi
        printf 'Please enter a number from 1 to %s.\n' "$max_choice" >&2
        ;;
    esac
  done
}

prompt_yes_no() {
  local prompt="$1"
  local default="${2:-n}"
  local answer

  while true; do
    if [[ "$default" == "y" ]]; then
      printf '%s [Y/n] ' "$prompt"
    else
      printf '%s [y/N] ' "$prompt"
    fi

    IFS= read -r answer
    answer="${answer:-$default}"
    case "$answer" in
      y|Y|yes|YES) return 0 ;;
      n|N|no|NO) return 1 ;;
      *) printf 'Please answer yes or no.\n' ;;
    esac
  done
}

validate_https_url() {
  local url="$1"

  [[ "$url" == https://* ]] || return 1
  [[ "$url" != *$'\n'* ]] || return 1
  [[ "$url" != *$'\r'* ]] || return 1
  [[ "$url" != *$'\t'* ]] || return 1
  [[ "$url" != *' '* ]] || return 1
}

open_meteo_url() {
  local lat="$1"
  local lon="$2"
  printf 'https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,weather_code&timezone=auto\n' "$lat" "$lon"
}

use_default_weather() {
  WEATHER_PLACE="$DEFAULT_WEATHER_PLACE"
  WEATHER_URL="$DEFAULT_WEATHER_URL"
  info "Using default Seoul weather."
}

custom_weather_url() {
  local label
  local url

  cat <<'TEXT'

Paste a complete HTTPS weather JSON URL.

Supported examples:
  Open-Meteo:
    https://api.open-meteo.com/v1/forecast?latitude=37.5665&longitude=126.9780&current=temperature_2m,weather_code&timezone=auto

  OpenWeather:
    https://api.openweathermap.org/data/2.5/weather?q=Seoul&appid=YOUR_KEY&units=metric

Open-Meteo does not require an API key. OpenWeather requires your own key.
Use metric units if you want Celsius.

Important:
  If you leave the API URL blank, it will be handled as:
    default: Seoul

Press Enter at the URL prompt to skip this step and use the default Seoul
Open-Meteo weather instead.
TEXT

  while true; do
    printf '\nCity or town label shown in the panel [Custom]: '
    IFS= read -r label
    label="${label:-Custom}"
    break
  done

  while true; do
    printf 'Weather API URL [blank = default: Seoul]: '
    IFS= read -r url
    if [[ -z "$url" ]]; then
      use_default_weather
      return 0
    fi
    if validate_https_url "$url"; then
      WEATHER_PLACE="$label"
      WEATHER_URL="$url"
      return 0
    fi
    printf 'Please enter a single-line HTTPS URL. The URL is stored as data only and is never executed by this script.\n'
  done
}

set_builtin_city() {
  local label="$1"
  local lat="$2"
  local lon="$3"

  WEATHER_PLACE="$label"
  WEATHER_URL="$(open_meteo_url "$lat" "$lon")"
}

city_menu() {
  local continent="$1"
  shift
  local entries=("$@")
  local choice
  local entry
  local label
  local lat
  local lon

  printf '\n%s major cities:\n' "$continent"
  local i=1
  for entry in "${entries[@]}"; do
    IFS='|' read -r label lat lon <<< "$entry"
    printf '  %d. %s\n' "$i" "$label"
    ((i++))
  done
  printf '  6. Other / enter a direct weather API URL\n'

  choice="$(prompt_choice 'Choose a city:' 6)"
  if [[ "$choice" == "6" ]]; then
    custom_weather_url
    return 0
  fi

  entry="${entries[$((choice - 1))]}"
  IFS='|' read -r label lat lon <<< "$entry"
  set_builtin_city "$label" "$lat" "$lon"
}

choose_builtin_weather() {
  local choice

  cat <<'TEXT'

Choose a continent:
  1. Asia
  2. America
  3. Europe
  4. Africa
  5. Oceania
TEXT

  choice="$(prompt_choice 'Continent:' 5)"
  case "$choice" in
    1)
      city_menu "Asia" \
        "Seoul|37.5665|126.9780" \
        "Shanghai|31.2304|121.4737" \
        "Beijing|39.9042|116.4074" \
        "Tokyo|35.6762|139.6503" \
        "Singapore|1.3521|103.8198"
      ;;
    2)
      city_menu "America" \
        "New York|40.7128|-74.0060" \
        "Los Angeles|34.0522|-118.2437" \
        "Mexico City|19.4326|-99.1332" \
        "Sao Paulo|-23.5505|-46.6333" \
        "Toronto|43.6532|-79.3832"
      ;;
    3)
      city_menu "Europe" \
        "London|51.5072|-0.1276" \
        "Paris|48.8566|2.3522" \
        "Berlin|52.5200|13.4050" \
        "Madrid|40.4168|-3.7038" \
        "Rome|41.9028|12.4964"
      ;;
    4)
      city_menu "Africa" \
        "Cairo|30.0444|31.2357" \
        "Lagos|6.5244|3.3792" \
        "Nairobi|-1.2921|36.8219" \
        "Johannesburg|-26.2041|28.0473" \
        "Casablanca|33.5731|-7.5898"
      ;;
    5)
      city_menu "Oceania" \
        "Sydney|-33.8688|151.2093" \
        "Melbourne|-37.8136|144.9631" \
        "Auckland|-36.8509|174.7645" \
        "Brisbane|-27.4698|153.0251" \
        "Perth|-31.9523|115.8613"
      ;;
  esac
}

choose_weather() {
  cat <<'TEXT'

Weather setup

Do you want to enter a direct weather API URL?
  1. Yes, I have an Open-Meteo/OpenWeather URL or API-key URL
  2. No, choose a built-in Open-Meteo city
  3. Skip, use the default Seoul weather
TEXT

  case "$(prompt_choice 'Your choice:' 3)" in
    1) custom_weather_url ;;
    2) choose_builtin_weather ;;
    3) use_default_weather ;;
  esac
}

write_panel_config() {
  local config_dir="$1"
  local config_file="$config_dir/panel.ini"

  install -d -m 0700 -o "$TARGET_USER" -g "$TARGET_GROUP" "$config_dir"

  if [[ -f "$config_file" ]]; then
    cp -p -- "$config_file" "$config_file.backup.$(date +%Y%m%d-%H%M%S)"
  fi

  cat > "$BUILD_DIR/panel.ini" <<EOF
[weather]
place=$WEATHER_PLACE
url=$WEATHER_URL

[icons]
1_name=utilities-terminal
1_command=exo-open --launch TerminalEmulator
1_tooltip=Terminal
2_name=web-browser
2_command=exo-open --launch WebBrowser
2_tooltip=Browser
3_name=system-file-manager
3_command=exo-open --launch FileManager
3_tooltip=Files
4_name=preferences-system
4_command=xfce4-settings-manager
4_tooltip=Settings
EOF

  install -m 0600 -o "$TARGET_USER" -g "$TARGET_GROUP" "$BUILD_DIR/panel.ini" "$config_file"
}

write_autostart() {
  local autostart_dir="$TARGET_HOME/.config/autostart"
  local autostart_file="$autostart_dir/$AUTOSTART_NAME"

  install -d -m 0755 -o "$TARGET_USER" -g "$TARGET_GROUP" "$autostart_dir"

  cat > "$BUILD_DIR/$AUTOSTART_NAME" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=myPanel
Comment=Translucent Xfce desktop status panel
Exec=$INSTALL_PATH
TryExec=$INSTALL_PATH
Terminal=false
StartupNotify=false
OnlyShowIn=XFCE;
Hidden=false
EOF

  install -m 0644 -o "$TARGET_USER" -g "$TARGET_GROUP" "$BUILD_DIR/$AUTOSTART_NAME" "$autostart_file"
}

install_packages() {
  if ! command -v pacman >/dev/null 2>&1 || [[ ! -f /etc/arch-release ]]; then
    die "This installer supports Arch Linux only."
  fi

  if prompt_yes_no "Install required Arch packages with pacman now?" "y"; then
    pacman -S --needed "${REQUIRED_PACKAGES[@]}"
  else
    info "Skipping package installation. Build may fail if dependencies are missing."
  fi
}

build_panel() {
  local compiler

  if command -v clang >/dev/null 2>&1; then
    compiler="clang"
  elif command -v gcc >/dev/null 2>&1; then
    compiler="gcc"
  else
    die "No C compiler found. Install clang or gcc."
  fi

  pkg-config --exists gtk+-3.0 || die "gtk+-3.0 development files not found. Install gtk3 and pkgconf."

  info "Building $APP_NAME with $compiler"
  "$compiler" -O2 -pipe "$SCRIPT_DIR/myPanel.c" -o "$BUILD_DIR/myPanel" $(pkg-config --cflags --libs gtk+-3.0)

  if [[ -x "$INSTALL_PATH" ]]; then
    cp -p -- "$INSTALL_PATH" "$INSTALL_PATH.backup.$(date +%Y%m%d-%H%M%S)"
  fi

  install -m 0755 "$BUILD_DIR/myPanel" "$INSTALL_PATH"
}

try_enable_compositor() {
  if ! command -v xfconf-query >/dev/null 2>&1; then
    return 0
  fi

  if [[ -z "${DISPLAY:-}" ]]; then
    info "DISPLAY is not set, so compositor enabling was skipped."
    printf 'Enable it later in Xfce: Settings Manager -> Window Manager Tweaks -> Compositor.\n'
    return 0
  fi

  if runuser -u "$TARGET_USER" -- env DISPLAY="$DISPLAY" XAUTHORITY="${XAUTHORITY:-$TARGET_HOME/.Xauthority}" \
      xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null 2>&1; then
    info "Xfce compositor was enabled for transparency."
  else
    info "Could not change compositor settings automatically."
    printf 'Enable it manually: Settings Manager -> Window Manager Tweaks -> Compositor.\n'
  fi
}

start_panel_now() {
  if [[ -z "${DISPLAY:-}" ]]; then
    info "DISPLAY is not set, so $APP_NAME will start on next Xfce login."
    return 0
  fi

  if prompt_yes_no "Start or restart myPanel now in this Xfce session?" "y"; then
    pkill -u "$TARGET_USER" -x myPanel 2>/dev/null || true
    runuser -u "$TARGET_USER" -- env DISPLAY="$DISPLAY" XAUTHORITY="${XAUTHORITY:-$TARGET_HOME/.Xauthority}" \
      setsid -f "$INSTALL_PATH" >/tmp/myPanel.log 2>&1 || true
    info "Started $APP_NAME. Runtime log: /tmp/myPanel.log"
  fi
}

main() {
  cat <<'TEXT'
my_linux_panel installer

This program is extremely optimized for Arch Linux + Xfce4 on X11.
It installs a translucent desktop panel and creates an Xfce autostart entry.

sudo/root is required because this installer can install Arch packages and
places the binary in /usr/local/bin. Run it as:

  sudo ./install.sh
TEXT

  if (( EUID != 0 )); then
    die "sudo is required. Please run: sudo ./install.sh"
  fi

  if [[ -z "${SUDO_USER:-}" || "$SUDO_USER" == "root" ]]; then
    die "Run this with sudo from your normal Xfce desktop user, not from a root shell."
  fi

  TARGET_USER="$SUDO_USER"
  TARGET_GROUP="$(id -gn "$TARGET_USER")"
  TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"

  [[ -n "$TARGET_HOME" && -d "$TARGET_HOME" ]] || die "Could not resolve home directory for $TARGET_USER."

  if [[ "${XDG_CURRENT_DESKTOP:-}" != *XFCE* && "${DESKTOP_SESSION:-}" != xfce* ]]; then
    info "Current desktop does not look like Xfce."
    printf 'This panel is intentionally optimized for Xfce4/X11. Continue only if this is your target desktop.\n'
    prompt_yes_no "Continue anyway?" "n" || exit 0
  fi

  install_packages
  choose_weather
  build_panel
  write_panel_config "$TARGET_HOME/.config/myPanel"
  write_autostart
  try_enable_compositor
  start_panel_now

  cat <<EOF

Done.

Installed binary:
  $INSTALL_PATH

User config:
  $TARGET_HOME/.config/myPanel/panel.ini

Xfce autostart:
  $TARGET_HOME/.config/autostart/$AUTOSTART_NAME
EOF
}

main "$@"
