rviz_view_publisher
===========================
This repo is a modification of the original rviz_animated_view_controller. It is modified to publish the current rviz view (position and orientation) as a pose message in 10Hz (you can modify to your needs).

It is tested to build and run in Ubuntu 16.04 and ROS Kinetic.

Installing
---------------------------
clone and build:
```
cd <your_catkin_ws_dir>/src
git clone https://github.com/caomuqing/rviz_view_publisher
git clone https://github.com/ros-visualization/view_controller_msgs
cd ..
catkin build
source devel/setup.bash
```

Launching
---------------------------

You may launch a pre-configured rviz view:
```
roslaunch rviz_view_publisher demo.launch
```
Or you can open a clean rviz and go to Panels->Views and select Animated(rviz_animated_view_controller) in the right-side panel. Then you will be able to see a topic called `/rviz_view` in your `rostopic list`.