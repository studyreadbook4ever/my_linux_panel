#!/usr/bin/env bash
set -euo pipefail

APP_NAME="myPanel"
INSTALL_PATH="/usr/local/bin/myPanel"
SPOTIFY_FOCUS_PATH="/usr/local/bin/myPanel-spotify-focus"
AUTOSTART_NAME="mypanel.desktop"
DEFAULT_WEATHER_PLACE="Seoul"
DEFAULT_WEATHER_URL="https://api.open-meteo.com/v1/forecast?latitude=37.5665&longitude=126.9780&current=temperature_2m,weather_code&timezone=auto"
COMMON_REQUIRED_PACKAGES=(
  base-devel
  gtk3
  pkgconf
  curl
  exo
  xfce4-settings
  xfwm4
  xfconf
  clang
  libx11
  spotify-launcher
  playerctl
  libnotify
)
PIPEWIRE_AUDIO_PACKAGES=(
  pipewire-alsa
)
PULSE_AUDIO_PACKAGES=(
  alsa-plugins
  pulseaudio-alsa
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

package_installed() {
  pacman -Qq "$1" >/dev/null 2>&1
}

alsa_default_uses() {
  local backend="$1"
  local paths=()
  local conf

  [[ -f /etc/asound.conf ]] && paths+=(/etc/asound.conf)
  if [[ -d /etc/alsa/conf.d ]]; then
    while IFS= read -r conf; do
      paths+=("$conf")
    done < <(find /etc/alsa/conf.d -maxdepth 1 -name '*.conf' | sort)
  fi
  [[ -f "$TARGET_HOME/.asoundrc" ]] && paths+=("$TARGET_HOME/.asoundrc")

  ((${#paths[@]} > 0)) || return 1

  awk -v backend="$backend" '
    /^[[:space:]]*(pcm|ctl)\.!default[[:space:]]*\{/ {
      in_default = 1
      next
    }
    in_default && /^[[:space:]]*}/ {
      in_default = 0
      next
    }
    in_default && $1 == "type" && $2 == backend {
      found = 1
    }
    END {
      exit found ? 0 : 1
    }
  ' "${paths[@]}"
}

target_command_succeeds() {
  local runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$TARGET_UID}"

  if [[ -d "$runtime_dir" ]]; then
    runuser -u "$TARGET_USER" -- env XDG_RUNTIME_DIR="$runtime_dir" "$@" >/dev/null 2>&1
  else
    runuser -u "$TARGET_USER" -- "$@" >/dev/null 2>&1
  fi
}

pipewire_active() {
  command -v wpctl >/dev/null 2>&1 && target_command_succeeds wpctl status
}

pulse_active() {
  command -v pactl >/dev/null 2>&1 && target_command_succeeds pactl info
}

set_audio_stack() {
  AUDIO_STACK="$1"
  AUDIO_STACK_REASON="$2"
  case "$AUDIO_STACK" in
    pipewire)
      AUDIO_PACKAGES=("${PIPEWIRE_AUDIO_PACKAGES[@]}")
      ;;
    pulse)
      AUDIO_PACKAGES=("${PULSE_AUDIO_PACKAGES[@]}")
      ;;
    skip)
      AUDIO_PACKAGES=()
      ;;
    *)
      die "Unknown audio stack: $AUDIO_STACK"
      ;;
  esac
}

choose_audio_stack() {
  cat <<'TEXT'

Spotify audio setup

The installer could not confidently detect the ALSA audio bridge to use.
Choose the bridge that matches your current desktop audio stack:

  1. PipeWire ALSA bridge (pipewire-alsa)
  2. PulseAudio ALSA bridge (alsa-plugins + pulseaudio-alsa)
  3. Skip audio bridge packages
TEXT

  case "$(prompt_choice 'Audio choice:' 3)" in
    1) set_audio_stack "pipewire" "selected by user" ;;
    2) set_audio_stack "pulse" "selected by user" ;;
    3) set_audio_stack "skip" "selected by user" ;;
  esac
}

detect_audio_packages() {
  AUDIO_STACK=""
  AUDIO_STACK_REASON=""
  AUDIO_PACKAGES=()

  if alsa_default_uses pipewire; then
    set_audio_stack "pipewire" "ALSA default points to PipeWire"
  elif alsa_default_uses pulse; then
    set_audio_stack "pulse" "ALSA default points to PulseAudio"
  elif pipewire_active; then
    set_audio_stack "pipewire" "wpctl can talk to the target user's PipeWire session"
  elif pulse_active; then
    set_audio_stack "pulse" "pactl can talk to the target user's PulseAudio server"
  elif package_installed pipewire-alsa; then
    set_audio_stack "pipewire" "pipewire-alsa is already installed"
  elif package_installed pulseaudio-alsa; then
    set_audio_stack "pulse" "pulseaudio-alsa is already installed"
  elif package_installed pipewire-audio || package_installed pipewire; then
    set_audio_stack "pipewire" "PipeWire packages are installed"
  elif package_installed pulseaudio; then
    set_audio_stack "pulse" "PulseAudio is installed"
  else
    choose_audio_stack
  fi

  case "$AUDIO_STACK" in
    pipewire)
      info "Spotify audio bridge: PipeWire (${AUDIO_STACK_REASON})."
      printf 'Will install only the PipeWire ALSA bridge package: %s\n' "${AUDIO_PACKAGES[*]}"
      ;;
    pulse)
      info "Spotify audio bridge: PulseAudio (${AUDIO_STACK_REASON})."
      printf 'Will install only PulseAudio ALSA bridge packages: %s\n' "${AUDIO_PACKAGES[*]}"
      printf 'The PulseAudio sound server package itself is not installed by this installer.\n'
      ;;
    skip)
      info "Spotify audio bridge package installation was skipped (${AUDIO_STACK_REASON})."
      ;;
  esac
}

install_packages() {
  if ! command -v pacman >/dev/null 2>&1 || [[ ! -f /etc/arch-release ]]; then
    die "This installer supports Arch Linux only."
  fi

  detect_audio_packages
  local packages=("${COMMON_REQUIRED_PACKAGES[@]}" "${AUDIO_PACKAGES[@]}")

  if prompt_yes_no "Install required Arch packages with pacman now?" "y"; then
    pacman -S --needed "${packages[@]}"
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

install_spotify_helpers() {
  local compiler
  local local_bin="$TARGET_HOME/.local/bin"
  local spotify_wrapper="$local_bin/spotify"
  local playerctl_wrapper="$local_bin/playerctl"

  if command -v clang >/dev/null 2>&1; then
    compiler="clang"
  elif command -v gcc >/dev/null 2>&1; then
    compiler="gcc"
  else
    die "No C compiler found. Install clang or gcc."
  fi

  [[ -f "$SCRIPT_DIR/tools/spotify-window-activate.c" ]] || \
    die "Missing tools/spotify-window-activate.c"

  info "Building Spotify focus helper with $compiler"
  "$compiler" -O2 -pipe "$SCRIPT_DIR/tools/spotify-window-activate.c" \
    -o "$BUILD_DIR/myPanel-spotify-focus" -lX11
  install -m 0755 "$BUILD_DIR/myPanel-spotify-focus" "$SPOTIFY_FOCUS_PATH"

  install -d -m 0755 -o "$TARGET_USER" -g "$TARGET_GROUP" "$local_bin"

  cat > "$BUILD_DIR/spotify" <<EOF
#!/usr/bin/env sh
set -eu

focus_helper="\${SPOTIFY_FOCUS_HELPER:-$SPOTIFY_FOCUS_PATH}"
launcher="\${SPOTIFY_LAUNCHER:-}"
data_home="\${XDG_DATA_HOME:-\$HOME/.local/share}"
log_file="\${XDG_CACHE_HOME:-\$HOME/.cache}/spotify-linux-wrapper.log"

if [ -z "\$launcher" ]; then
  launcher="\$(command -v spotify-launcher || true)"
fi

mkdir -p "\$(dirname "\$log_file")"
printf '[%s] spotify wrapper invoked: %s\n' "\$(date -Is)" "\$*" >>"\$log_file"

focus_spotify() {
  [ -x "\$focus_helper" ] && "\$focus_helper" >/dev/null 2>&1
}

find_spotify_binary() {
  for candidate in \
    "\${SPOTIFY_LINUX_BINARY:-}" \
    "\$data_home/spotify-launcher/install/usr/bin/spotify" \
    "\$data_home/spotify-launcher/install/usr/share/spotify/spotify"
  do
    [ -n "\$candidate" ] && [ -x "\$candidate" ] && {
      printf '%s\n' "\$candidate"
      return 0
    }
  done
  return 1
}

run_spotify_binary() {
  spotify_bin="\$1"
  shift
  spotify_dir="\$(dirname "\$spotify_bin")"
  printf '[%s] launching cached Spotify Linux binary: %s\n' \
    "\$(date -Is)" "\$spotify_bin" >>"\$log_file"
  (cd "\$spotify_dir" && setsid -f "\$spotify_bin" "\$@" >>"\$log_file" 2>&1) || true
}

if [ "\$#" -eq 0 ] && focus_spotify; then
  exit 0
fi

spotify_bin="\$(find_spotify_binary || true)"
if [ -z "\$spotify_bin" ] && [ -n "\$launcher" ]; then
  printf '[%s] cached Spotify binary missing; asking spotify-launcher to prepare it\n' \
    "\$(date -Is)" >>"\$log_file"
  "\$launcher" --no-exec >>"\$log_file" 2>&1 || true
  spotify_bin="\$(find_spotify_binary || true)"
fi

if [ -n "\$spotify_bin" ]; then
  run_spotify_binary "\$spotify_bin" "\$@"
elif [ -n "\$launcher" ]; then
  printf '[%s] falling back to spotify-launcher\n' "\$(date -Is)" >>"\$log_file"
  setsid -f "\$launcher" "\$@" >>"\$log_file" 2>&1 || true
else
  printf '%s\n' "spotify: spotify-launcher is not installed" >&2
  exit 127
fi

i=0
while [ "\$i" -lt 30 ]; do
  if focus_spotify; then
    exit 0
  fi
  i=\$((i + 1))
  sleep 0.2
done

exit 0
EOF

  install -m 0755 -o "$TARGET_USER" -g "$TARGET_GROUP" \
    "$BUILD_DIR/spotify" "$spotify_wrapper"

  if [[ ! -x /usr/bin/playerctl ]]; then
    cat > "$BUILD_DIR/playerctl" <<'EOF'
#!/usr/bin/env sh
set -eu

if [ -x /usr/bin/playerctl ]; then
  exec /usr/bin/playerctl "$@"
fi

service="${PLAYERCTL_PLAYER:-org.mpris.MediaPlayer2.spotify}"
path="/org/mpris/MediaPlayer2"
iface="org.mpris.MediaPlayer2.Player"

usage() {
  cat >&2 <<'TEXT'
playerctl fallback for Spotify is installed locally.
Supported commands: -l, play, pause, play-pause, next, previous, stop, status, metadata
Install the full package for every playerctl feature: sudo pacman -S --needed playerctl
TEXT
}

call_method() {
  busctl --user call "$service" "$path" "$iface" "$1" >/dev/null
}

get_property() {
  busctl --user get-property "$service" "$path" "$iface" "$1"
}

metadata_value() {
  key="$1"
  get_property Metadata | tr '"' '\n' | awk -v key="$key" '
    $0 == key { getline; getline; print; exit }
  '
}

player_present() {
  busctl --user list 2>/dev/null | grep -q "org.mpris.MediaPlayer2.spotify"
}

player_identity() {
  busctl --user get-property "$service" /org/mpris/MediaPlayer2 \
    org.mpris.MediaPlayer2 Identity 2>/dev/null |
    sed -E 's/^s "(.*)"$/\1/'
}

playback_status() {
  get_property PlaybackStatus 2>/dev/null | sed -E 's/^s "(.*)"$/\1/'
}

cmd="${1:-}"
case "$cmd" in
  -l|--list-all)
    if player_present; then
      printf '%s\n' spotify
    fi
    ;;
  play)
    call_method Play
    ;;
  pause)
    call_method Pause
    ;;
  play-pause|playpause)
    call_method PlayPause
    ;;
  next)
    call_method Next
    ;;
  previous)
    call_method Previous
    ;;
  stop)
    call_method Stop
    ;;
  status)
    playback_status
    ;;
  metadata)
    if [ "${2:-}" = "--format" ] && [ -n "${3:-}" ]; then
      player_name="$(player_identity || true)"
      status="$(playback_status || true)"
      artist="$(metadata_value xesam:artist || true)"
      title="$(metadata_value xesam:title || true)"
      album="$(metadata_value xesam:album || true)"
      printf '%s\n' "$3" |
        awk \
          -v playerName="$player_name" \
          -v status="$status" \
          -v artist="$artist" \
          -v title="$title" \
          -v album="$album" '
            {
              gsub(/\{\{[ ]*playerName[ ]*\}\}/, playerName)
              gsub(/\{\{[ ]*status[ ]*\}\}/, status)
              gsub(/\{\{[ ]*artist[ ]*\}\}/, artist)
              gsub(/\{\{[ ]*title[ ]*\}\}/, title)
              gsub(/\{\{[ ]*album[ ]*\}\}/, album)
              print
            }
          '
    elif [ -n "${2:-}" ]; then
      metadata_value "$2"
    else
      get_property Metadata
    fi
    ;;
  -h|--help|"")
    usage
    [ -n "$cmd" ]
    ;;
  *)
    usage
    exit 2
    ;;
esac
EOF

    install -m 0755 -o "$TARGET_USER" -g "$TARGET_GROUP" \
      "$BUILD_DIR/playerctl" "$playerctl_wrapper"
  fi
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
  TARGET_UID="$(id -u "$TARGET_USER")"
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
  install_spotify_helpers
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
