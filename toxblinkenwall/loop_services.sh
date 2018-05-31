#! /bin/bash

function clean_up
{
	pkill toxblinkenwall
	sleep 2
	pkill -9 toxblinkenwall
	pkill -9 toxblinkenwall
	pkill -f ext_keys.py
	# Perform program exit cleanup of framebuffer
	scripts/stop_loading_endless.sh
	scripts/cleanup_fb.sh
	exit
}

cd $(dirname "$0")
export LD_LIBRARY_PATH=~/inst/lib/

HD_FROM_CAM="" # set to "-f" for 720p video
# you can switch it also later when Tox is running

# ---- only for RASPI ----
if [ "$IS_ON""x" == "RASPI""x" ]; then
	# camera module is never loaded automatically, why is that?
	sudo modprobe bcm2835_v4l2
	# nice now the module suddenly has a new name?
	sudo modprobe bcm2835-v4l2
	# stop gfx UI
	# sudo /etc/init.d/lightdm start
	# sleep 2

	sudo sed -i -e 's#BLANK_TIME=.*#BLANK_TIME=0#' /etc/kbd/config
	sudo sed -i -e 's#POWERDOWN_TIME=.*#POWERDOWN_TIME=0#' /etc/kbd/config
	sudo setterm -blank 0 > /dev/null 2>&1
	sudo setterm -powerdown 0 > /dev/null 2>&1

	openvt -- sudo sh -c "/bin/chvt 1 >/dev/null 2>/dev/null"
	sudo sh -c "TERM=linux setterm -blank 0 >/dev/tty0"
fi
# ---- only for RASPI ----

trap clean_up SIGHUP SIGINT SIGTERM SIGKILL

chmod u+x scripts/*.sh
chmod u+x toxblinkenwall
chmod u+x ext_keys_scripts/ext_keys.py
chmod a+x udev2.sh udev.sh toggle_alsa.sh
scripts/stop_loading_endless.sh
scripts/stop_image_endless.sh
scripts/init.sh
sleep 1
scripts/create_gfx.sh

while [ 1 == 1 ]; do
	scripts/stop_loading_endless.sh
	scripts/stop_image_endless.sh
	. scripts/vars.sh

	pkill -f ext_keys.py

	v4l2-ctl -d "$video_device" -v width=1280,height=720,pixelformat=YV12
        v4l2-ctl -d "$video_device" -p 25

	cd ext_keys_scripts
	./ext_keys.py &
	cd ..

    # ---- only for RASPI ----
    if [ "$IS_ON""x" == "RASPI""x" ]; then
            sudo ./toggle_alsa.sh 0
    fi
    # ---- only for RASPI ----

	setterm -cursor off
	./toxblinkenwall $HD_FROM_CAM -u "$fb_device" -j "$BKWALL_WIDTH" -k "$BKWALL_HEIGHT" -d "$video_device"
	sleep 2

    # ---- only for RASPI ----
    if [ "$IS_ON""x" == "RASPI""x" ]; then
            sudo ./toggle_alsa.sh 0
    fi
    # ---- only for RASPI ----
done

