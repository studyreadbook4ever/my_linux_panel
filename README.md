<img width="345" height="400" alt="image" src="https://github.com/user-attachments/assets/1d70cbfb-5f13-4052-94a3-b81a04f7c12b" />



# my_linux_panel

`my_linux_panel` is a tiny Vista-like translucent desktop panel for Arch Linux
+ Xfce4 on X11.(recommand for 'only Xfce4')

It starts as one small `menu` button in the top-left corner. Pressing `menu`
opens a glass panel with system status text, weather, Spotify now playing,
native Spotify launch, shortcut icons, a weather-reset icon, settings, and a
guarded power dialog.

The glass area is click-through on X11. Only these widgets catch clicks:

- `menu` / `close`
- the top-right power button
- shell, internet, Spotify, file, weather settings, and settings icons

## Quick Install on Arch Linux + Xfce4

Run the installer from this repository:

```sh
sudo ./install.sh
```

The installer intentionally requires `sudo`. If you run it without `sudo`, it
exits before changing anything. Root is required because the installer can
install Arch packages and places the final binary in:

```text
/usr/local/bin/myPanel
```

The installer is optimized for Xfce4/X11. It will warn you if the current
desktop does not look like Xfce.

## What install.sh Does

`install.sh`:

1. Checks that it is running with `sudo`.
2. Checks that it is running on Arch Linux.
3. Offers to install required packages with `pacman`.
4. Builds `myPanel` with `clang` or `gcc`.
5. Installs the binary to `/usr/local/bin/myPanel`.
6. Creates `~/.config/myPanel/panel.ini` for the real desktop user.
7. Creates `~/.config/autostart/mypanel.desktop` for Xfce login startup.
8. Tries to enable the Xfce compositor for real transparency.
9. Installs `spotify-launcher`, `playerctl`, `libnotify`, and the PulseAudio
   ALSA bridge packages so the Spotify button can use the native desktop client
   and MPRIS-friendly Linux media stack.
10. Builds a tiny X11 Spotify focus helper and writes a `~/.local/bin/spotify`
    wrapper for the desktop user. The wrapper focuses an existing Spotify
    window, or starts `spotify-launcher` when Spotify is not running. On
    PulseAudio-first Xfce sessions, it also makes Spotify skip the half-present
    PipeWire path and use PulseAudio playback instead.
11. Offers to start or restart the panel immediately.

The weather config is written with file mode `0600`, so API keys in URLs are not
world-readable.

## Installer Weather Flow

The installer asks:

```text
Do you want to enter a direct weather API URL?
  1. Yes, I have an Open-Meteo/OpenWeather URL or API-key URL
  2. No, choose a built-in Open-Meteo city
  3. Skip, use the default Seoul weather
```

If you choose `1`, paste a complete HTTPS weather JSON URL. The direct API
input screen clearly says that a blank API URL is handled as `default: Seoul`.
Press Enter at the URL prompt to skip and use the default Seoul Open-Meteo
weather.

Open-Meteo example, no API key needed:

```text
https://api.open-meteo.com/v1/forecast?latitude=37.5665&longitude=126.9780&current=temperature_2m,weather_code&timezone=auto
```

OpenWeather example, API key needed:

```text
https://api.openweathermap.org/data/2.5/weather?q=Seoul&appid=YOUR_KEY&units=metric
```

If you choose `2`, the installer shows:

```text
Choose a continent:
  1. Asia
  2. America
  3. Europe
  4. Africa
  5. Oceania
```

Each continent opens its own six-choice city menu: five built-in cities plus
`6. Other`, which returns to the direct API URL input screen. Asia starts with
Seoul because this project is tuned from Seoul first:

```text
1. Seoul
2. Shanghai
3. Beijing
4. Tokyo
5. Singapore
6. Other / enter a direct weather API URL
```

Choosing `Other` on any continent sends you to the same direct weather API URL
input used by option `1`.

## Runtime UI

Closed state:

- Only the small `menu` button is visible.

Open state:

- System status text: time, CPU load, network latency, uptime, battery if present.
- Top-right power button aligned with the settings icon column.
- Weather area: weather icon and current weather label.
- Shortcut row order: shell, internet, Spotify, file, weather settings, settings.
- Spotify button: launches the local `spotify` wrapper first. That wrapper
  focuses an existing Spotify window or starts the native `spotify-launcher`
  client when Spotify is not running. The wrapper sets `PIPEWIRE_REMOTE` to a
  harmless missing remote by default, which makes the current Spotify client
  fall back to PulseAudio on Xfce systems where PipeWire is present but not the
  active audio stack. If neither command exists, the panel requests
  installation of `spotify-launcher` and `playerctl` through `pkexec pacman`,
  then starts Spotify after installation.
  The icon prefers the official Spotify app icon from the installed system icon
  theme, without bundling Spotify logo files into this repository.
- Embedded Spotify player: placed directly under the shortcut row. It reads
  `org.mpris.MediaPlayer2.spotify` over DBus and shows album art, title,
  artist, album, track progress, and previous/play-pause/next controls without
  embedding the fragile Spotify app window itself.
  This player is a custom GTK view built from Spotify/MPRIS metadata and
  commands, not a native Spotify-provided embedded UI. Because of that, some
  GTK widget styling can still feel more Linux-toolkit-like than macOS-native;
  future polish should focus on reducing default GTK widget chrome or drawing
  the compact media capsule manually.
- Weather settings icon: opens a weather setup dialog and lets you paste a new
  API URL without editing files manually.
- Power button: opens a separate dialog with `poweroff`, `suspend`,
  `hibernate`, and `shutdown` controls. Turn on the confirmation switch before
  a power action can run.

## Manual Build for Development

Install dependencies:

```sh
sudo pacman -S --needed base-devel gtk3 pkgconf curl alsa-plugins exo xfce4-settings xfwm4 xfconf clang libx11 pulseaudio pulseaudio-alsa spotify-launcher playerctl libnotify
```

Build:

```sh
clang -O2 -pipe myPanel.c -o myPanel $(pkg-config --cflags --libs gtk+-3.0)
```

`gcc` works too when the local GCC toolchain is healthy:

```sh
gcc -O2 -pipe myPanel.c -o myPanel $(pkg-config --cflags --libs gtk+-3.0)
```

Run locally:

```sh
./myPanel
```

## Xfce Transparency

The panel uses real RGBA transparency, so the Xfce compositor must be enabled.

GUI path:

```text
Settings Manager -> Window Manager Tweaks -> Compositor -> Enable display compositing
```

Terminal check:

```sh
xfconf-query -c xfwm4 -p /general/use_compositing
```

Enable it:

```sh
xfconf-query -c xfwm4 -p /general/use_compositing -s true
```

## Config File

The config file is:

```text
~/.config/myPanel/panel.ini
```

Weather:

```ini
[weather]
place=Seoul
url=https://api.open-meteo.com/v1/forecast?latitude=37.5665&longitude=126.9780&current=temperature_2m,weather_code&timezone=auto
```

Four configurable shortcut slots. They are rendered around the fixed Spotify
and weather-settings buttons:

```text
1 shell -> 2 internet -> Spotify -> 3 file -> weather settings -> 4 settings
```

```ini
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
```

For each shortcut:

- `*_name`: GTK icon theme name, such as `web-browser`, or image path like `~/Pictures/icon.png`.
- `*_command`: command to run when clicked.
- `*_tooltip`: hover text.

Restart `myPanel` after editing shortcut slots.

## Xfce Autostart

The installer creates:

```text
~/.config/autostart/mypanel.desktop
```

Expected contents:

```ini
[Desktop Entry]
Type=Application
Version=1.0
Name=myPanel
Comment=Translucent Xfce desktop status panel
Exec=/usr/local/bin/myPanel
TryExec=/usr/local/bin/myPanel
Terminal=false
StartupNotify=false
OnlyShowIn=XFCE;
Hidden=false
```

Manual GUI path:

```text
Settings Manager -> Session and Startup -> Application Autostart -> Add
```

Use:

- Name: `myPanel`
- Command: `/usr/local/bin/myPanel`
- Trigger: `on login`

## Restart

```sh
pkill -x myPanel
/usr/local/bin/myPanel &
```

Check what is running:

```sh
pgrep -af myPanel
```

## Troubleshooting

Installer says sudo is required:

```sh
sudo ./install.sh
```

Panel is black instead of translucent:

```sh
xfconf-query -c xfwm4 -p /general/use_compositing -s true
```

Weather says `install curl`:

```sh
sudo pacman -S --needed curl
```

Weather says `parse fail`:

- Make sure the URL returns JSON.
- For Open-Meteo, include `current=temperature_2m,weather_code`.
- For OpenWeather, include `units=metric`.

Shortcut icon is blank:

- Use a GTK icon name such as `utilities-terminal`, `web-browser`, or `preferences-system`.
- Or use a real image path such as `/home/YOUR_USER/Pictures/icon.png`.

Shortcut click does nothing:

```sh
sh -lc 'your command here'
```

Run the command manually first. If it fails in the terminal, it will fail from
the panel too.
