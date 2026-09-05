#!/bin/sh
# Install Iris as a user-level boot service (no sudo). Requires `loginctl enable-linger $USER`
# so the user session starts at boot; the humdan user on the Pi already has linger on.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
make -C "$DIR" iris_fb
mkdir -p ~/.config/systemd/user ~/.local/bin
cp "$DIR/iris-user.service" ~/.config/systemd/user/iris.service
install -m 755 "$DIR/iris-state" ~/.local/bin/iris-state
systemctl --user daemon-reload
systemctl --user enable iris.service
systemctl --user restart iris.service
sleep 2
systemctl --user --no-pager status iris.service | head -6
echo
echo "Iris installed as a user service. Hook Hermes to it:"
echo "  ln -s $DIR/plugin ~/.hermes/plugins/iris && hermes plugins enable iris"
