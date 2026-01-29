# XRoboToolkit-Orin-Video-Sender
Video Previewer/Encoder/Sender on Nvidia Jetson Orin Platform

![Screenshot](Docs/screenshot.png)
> Sender (Webcam): `./OrinVideoSender --preview --send --server 192.168.1.176 --port 12345`

> Receiver (Video-Viewer): TCP - 192.168.1.176 - 12345 - 1280x720

## Features

- Support Webcam and ZED cameras
- Preview
- H264 Encoding (via GStreamer)
- TCP/UDP sending w/ and w/o ASIO


## How to

- Setup necessary environment on Orin

- Build
```
# Update `Makefile` to choose the protocol [TCP/UDP], camera type [Webcam/ZED], w/ or w/o ASIO.
# Default: TCP w/o asio.

# install zmq
sudo apt-get install libzmq3-dev

make

./OrinVideoSender --help

# Listen to coming command from VR, 192.168.1.153 is the Orin IP address
# Add `--preview` to show the video on Orin if necessary 
./OrinVideoSender --listen 192.168.1.153:13579

# send the video stream to both VR via TCP and my own ubuntu via ZMQ
./OrinVideoSender --listen 192.168.1.153:13579 --zmq tcp://*:5555

# Direct send the video stream # 192.168.1.176 is the VR headset IP
# Add `--preview` to show the video on Orin if necessary 
./OrinVideoSender --send --server 192.168.1.176 --port 12345
```

## MuJoCo D435i (virtual camera)

This repo can send a MuJoCo virtual camera stream (e.g. `d435i_rgb`) via a
v4l2loopback device. The flow is:
1) MuJoCo renders frames and writes to `/dev/video10`
2) `OrinVideoSender` reads `/dev/video10` and streams to the Quest client

### Prereqs (host)

Create a v4l2loopback device on the host:
```
sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="mujoco_cam" exclusive_caps=1
ls -l /dev/video10
```

If running in Docker, pass the device through to the container:
```
devices:
  - /dev/video10:/dev/video10
```

After host reboot, the device may exist on the host but not in the container.
Verify inside the container:
```
ls -l /dev/video10
```
If it is missing, restart/recreate the container (or devcontainer) so Docker
re-attaches the device.

### MuJoCo side (container)

Ensure your MuJoCo sim is writing to `/dev/video10` (see
`unitree_mujoco/simulate_python/config.py` in the main workspace) and start it:
```
cd unitree_mujoco/simulate_python
python ./unitree_mujoco.py
```

### Run OrinVideoSender (container)

Command port: **13579**, stream port: **12345** (Quest defaults).

Mono MUJOCO stream:
```
./OrinVideoSender --cmd-listen 0.0.0.0:13579 --mujoco-mono
```

ZEDMINI SBS compatibility (2560x720 side-by-side):
```
./OrinVideoSender --cmd-listen 0.0.0.0:13579 --zed-sbs
```

### Quest client

- Select **MUJOCO** in the dropdown if using `--mujoco-mono`.
- Select **ZEDMINI** if using `--zed-sbs`.
- Set Host IP to the sender machine (e.g. `10.20.20.53`).

## One More Thing 

- For software encoding ffmpeg, please refer to [RobotVisionTest](https://github.com/XR-Robotics/RobotVision-PC/tree/main/VideoTransferPC/RobotVisionTest).

> Note: Hardware ffmpeg encoding is not availalbe yet.

> Note: Jetson Multimedia API is not in use yet.

- For encoded h264 stream receiver, please refer to [VideoPlayer](https://github.com/XR-Robotics/RobotVision-PC/tree/main/VideoTransferPC/VideoPlayer) [TCP Only].

- For a general video player, please refer to [Video-Viewer](https://github.com/XR-Robotics/XRoboToolkit-Native-Video-Viewer) [TCP/UDP].

- The encoded h264 stream can be also played in [Unity-Client](https://github.com/XR-Robotics/XRoboToolkit-Unity-Client) [TCP Only].
