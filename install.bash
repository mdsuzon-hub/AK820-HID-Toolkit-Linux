sudo modprobe uinput
echo uinput | sudo tee /etc/modules-load.d/uinput.conf

sudo cp 70-ak820.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
ls -l /dev/uinput          # should show an ACL (+) for your user; if not, replug the keyboard or relogin

cmake --build build -j && ./build/ak820-controller

# If volume still changes, tell me the output of:
for e in 7 8; do echo event$e; sudo evtest --query /dev/input/event$e EV_KEY KEY_VOLUMEUP; done
sudo evtest /dev/input/event8   # turn the knob, see which event node reports KEY_VOLUMEUP