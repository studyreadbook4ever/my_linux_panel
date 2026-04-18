# my_linux_panel
use on xfce4

download myPanel -> chmod +x on it -> execute! 

settings->session and startup click->current session and 'quit program' on xfce4-panel and save session for right usage

put this on ~/.config/autostart/mypanel.desktop
you can do it easily by this command on shell


mkdir -p "$HOME/.local/bin"

install -Dm755 /path/to/your_program "$HOME/.local/bin/mypanel"

[Desktop Entry]

Type=Application

Version=1.0

Name=myPanel

Comment=Minimal Xfce status panel

Exec=/home/yourname/.local/bin/mypanel

TryExec=/home/yourname/.local/bin/mypanel

Terminal=false

StartupNotify=false

OnlyShowIn=XFCE;

Hidden=false





systemd/logind.conf.d/ 에다가 설정해줄거

/lid-ignore.conf

[Login]

HandleLidSwitch=ignore

HandleLidSwitchExternalPower=ignore

HandleLidSwitchDocked=ignore

EOF

/10-idle.conf

[Login]

IdleAction=hibernate

IdleActionSec=10min
