# my_linux_panel
use on xfce4

settings->session and startup click->current session and 'quit program' on xfce4-panel and save session for right usage

download myPanel -> chmod +x on it -> execute! 

to make this permanently background running, get help by GPT gogo


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
