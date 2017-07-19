#! /bin/bash

qrcode_file_rgba=~/ToxBlinkenwall/toxblinkenwall/toxid.rgba

##### -------------------------------------
export FB_WIDTH=640
export FB_HEIGHT=480
export BKWALL_WIDTH=192
export BKWALL_HEIGHT=144
stride_=$(cat /sys/class/graphics/fb0/stride)
bits_per_pixel_=$(cat /sys/class/graphics/fb0/bits_per_pixel)
virtual_size=$(cat /sys/class/graphics/fb0/virtual_size)
tmp1=$[ $bits_per_pixel_ / 8 ]
export real_width=$[ $stride_ / $tmp1 ]
##### -------------------------------------

##### -------------------------------------
# pick first available framebuffer device
# change for your needs here!
export fb_device=$(ls -1 /dev/fb*|tail -1)
##### -------------------------------------


cat "$qrcode_file_rgba" > "$fb_device"