#!/bin/sh
# Install Iris as a boot service on the Pi LCD.
set -e
make -C "$(dirname "$0")" iris_fb
DIR="$(cd "$(dirname "$0")" && pwd)"
sudo cp "$DIR/iris.service" /etc/systemd/system/iris.service
sudo install -m 755 "$DIR/iris-state" /usr/local/bin/iris-state
sudo systemctl daemon-reload
sudo systemctl enable iris.service
sudo systemctl restart iris.service
sleep 2
systemctl --no-pager status iris.service | head -8
echo
echo "Iris installed. Control it with:  iris-state thinking  |  iris-state idle"
echo "Hook Hermes to it:  ln -s $DIR/plugin ~/.hermes/plugins/iris && hermes plugins enable iris"
