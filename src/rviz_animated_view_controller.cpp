/*
 * Copyright (c) 2009, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include "rviz_animated_view_controller/rviz_animated_view_controller.h"

#include "rviz/load_resource.h"
#include "rviz/uniform_string_stream.h"
#include "rviz/display_context.h"
#include "rviz/viewport_mouse_event.h"
#include "rviz/frame_manager.h"
#include "rviz/geometry.h"
#include "rviz/view_manager.h"
#include "rviz/render_panel.h"
#include "rviz/ogre_helpers/shape.h"
#include "rviz/properties/float_property.h"
#include "rviz/properties/vector_property.h"
#include "rviz/properties/bool_property.h"
#include "rviz/properties/tf_frame_property.h"
#include "rviz/properties/editable_enum_property.h"
#include "rviz/properties/ros_topic_property.h"

#include "view_controller_msgs/CameraPlacement.h"

#include <OGRE/OgreViewport.h>
#include <OGRE/OgreQuaternion.h>
#include <OGRE/OgreVector3.h>
#include <OGRE/OgreSceneNode.h>
#include <OGRE/OgreSceneManager.h>
#include <OGRE/OgreCamera.h>
#include <OGRE/OgreRenderWindow.h>

#include <tf/transform_datatypes.h>
#include <tf/LinearMath/Quaternion.h>
#include <geometry_msgs/PoseStamped.h>

namespace rviz_animated_view_controller
{
using namespace view_controller_msgs;
using namespace rviz;

// Strings for selecting control mode styles
static const std::string MODE_ORBIT = "Orbit";
static const std::string MODE_FPS = "FPS";

// Limits to prevent orbit controller singularity, but not currently used.
//static const Ogre::Radian PITCH_LIMIT_LOW  = Ogre::Radian(-Ogre::Math::HALF_PI + 0.02);
//static const Ogre::Radian PITCH_LIMIT_HIGH = Ogre::Radian( Ogre::Math::HALF_PI - 0.02);
static const Ogre::Radian PITCH_LIMIT_LOW  = Ogre::Radian( 0.02 );
static const Ogre::Radian PITCH_LIMIT_HIGH = Ogre::Radian( Ogre::Math::PI - 0.02);


// Some convenience functions for Ogre / geometry_msgs conversions
static inline Ogre::Vector3 vectorFromMsg(const geometry_msgs::Point &m) { return Ogre::Vector3(m.x, m.y, m.z); }
static inline Ogre::Vector3 vectorFromMsg(const geometry_msgs::Vector3 &m) { return Ogre::Vector3(m.x, m.y, m.z); }
static inline geometry_msgs::Point pointOgreToMsg(const Ogre::Vector3 &o)
{
  geometry_msgs::Point m;
  m.x = o.x; m.y = o.y; m.z = o.z;
  return m;
}
static inline void pointOgreToMsg(const Ogre::Vector3 &o, geometry_msgs::Point &m)  { m.x = o.x; m.y = o.y; m.z = o.z; }

static inline geometry_msgs::Vector3 vectorOgreToMsg(const Ogre::Vector3 &o)
{
  geometry_msgs::Vector3 m;
  m.x = o.x; m.y = o.y; m.z = o.z;
  return m;
}
static inline void vectorOgreToMsg(const Ogre::Vector3 &o, geometry_msgs::Vector3 &m) { m.x = o.x; m.y = o.y; m.z = o.z; }

// -----------------------------------------------------------------------------


AnimatedViewController::AnimatedViewController()
  : nh_("")
    , cam_movements_buffer_(100)
    , animate_(false)
    , dragging_(false)
    , render_frame_by_frame_(false)
    , target_fps_(60)
    , rendered_frames_counter_(0)
    , pause_animation_duration_(0.0)
{
  interaction_disabled_cursor_ = makeIconCursor( "package://rviz/icons/forbidden.svg" );

  mouse_enabled_property_ = new BoolProperty("Mouse Enabled", true,
                                   "Enables mouse control of the camera.",
                                   this);
  interaction_mode_property_ = new EditableEnumProperty("Control Mode", QString::fromStdString(MODE_ORBIT),
                                   "Select the style of mouse interaction.",
                                   this);
  interaction_mode_property_->addOptionStd(MODE_ORBIT);
  interaction_mode_property_->addOptionStd(MODE_FPS);
  interaction_mode_property_->setStdString(MODE_ORBIT);

  fixed_up_property_ = new BoolProperty( "Maintain Vertical Axis", true,
                                         "If enabled, the camera is not allowed to roll side-to-side.",
                                          this);
  attached_frame_property_ = new TfFrameProperty("Target Frame",
                                                 TfFrameProperty::FIXED_FRAME_STRING,
                                                 "TF frame the camera is attached to.",
                                                 this, NULL, true );
  eye_point_property_    = new VectorProperty( "Eye", Ogre::Vector3( 5, 5, 10 ),
                                              "Position of the camera.", this );
  focus_point_property_ = new VectorProperty( "Focus", Ogre::Vector3::ZERO,
                                              "Position of the focus/orbit point.", this );
  up_vector_property_ = new VectorProperty( "Up", Ogre::Vector3::UNIT_Z,
                                            "The vector which maps to \"up\" in the camera image plane.", this );
  distance_property_    = new FloatProperty( "Distance", getDistanceFromCameraToFocalPoint(),
                                             "The distance between the camera position and the focus point.",
                                             this );
  distance_property_->setMin( 0.01 );
  default_transition_time_property_ = new FloatProperty( "Transition Time", 0.5,
                                                         "The default time to use for camera transitions.",
                                                         this );
  camera_placement_topic_property_ = new RosTopicProperty("Placement Topic", "/rviz/camera_placement",
                                                          QString::fromStdString(ros::message_traits::datatype<view_controller_msgs::CameraPlacement>() ),
                                                          "Topic for CameraPlacement messages", this, SLOT(updateTopics()));

  camera_trajectory_topic_property_ = new RosTopicProperty("Trajectory Topic", "/rviz/camera_trajectory",
                                                           QString::fromStdString(
                                                             ros::message_traits::datatype<view_controller_msgs::CameraTrajectory>()),
                                                           "Topic for CameraTrajectory messages", this,
                                                           SLOT(updateTopics()));

  window_width_property_ = new FloatProperty("Window Width", 1000, "The width of the rviz visualization window in pixels.", this);
  window_height_property_ = new FloatProperty("Window Height", 1000, "The height of the rviz visualization window in pixels.", this);
  
  publish_view_images_property_ = new BoolProperty("Publish View Images During Animation", false, 
                                                   "If enabled, publishes images of what the user sees in the visualization window during an animation.", 
                                                   this);
  initializePublishers();
  initializeSubscribers();
}

AnimatedViewController::~AnimatedViewController()
{
    delete focal_shape_;
    context_->getSceneManager()->destroySceneNode( attached_scene_node_ );
}

void AnimatedViewController::updateTopics()
{
  placement_subscriber_  = nh_.subscribe<view_controller_msgs::CameraPlacement>
                              (camera_placement_topic_property_->getStdString(), 1,
                              boost::bind(&AnimatedViewController::cameraPlacementCallback, this, _1));
  
  trajectory_subscriber_ = nh_.subscribe<view_controller_msgs::CameraTrajectory>
                                (camera_trajectory_topic_property_->getStdString(), 1,
                                 boost::bind(&AnimatedViewController::cameraTrajectoryCallback, this, _1));
}

void AnimatedViewController::initializePublishers()
{
  current_camera_pose_publisher_ = nh_.advertise<geometry_msgs::PoseStamped>("/rviz/current_camera_pose", 1);
  finished_animation_publisher_ = nh_.advertise<std_msgs::Bool>("/rviz/finished_animation", 1);

  image_transport::ImageTransport it(nh_);
  camera_view_image_publisher_ = it.advertise("/rviz/view_image", 1);
}

void AnimatedViewController::initializeSubscribers()
{
  pause_animation_duration_subscriber_ = nh_.subscribe("/rviz/pause_animation_duration", 1,
                                                       &AnimatedViewController::pauseAnimationCallback, this);
}

void AnimatedViewController::pauseAnimationCallback(const std_msgs::Duration::ConstPtr& pause_duration_msg)
{
  pause_animation_duration_.sec = pause_duration_msg->data.sec;
  pause_animation_duration_.nsec = pause_duration_msg->data.nsec;
}

void AnimatedViewController::onInitialize()
{
    attached_frame_property_->setFrameManager( context_->getFrameManager() );
    attached_scene_node_ = context_->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    camera_->detachFromParent();
    attached_scene_node_->attachObject( camera_ );

    camera_->setProjectionType( Ogre::PT_PERSPECTIVE );

    focal_shape_ = new Shape(Shape::Sphere, context_->getSceneManager(), attached_scene_node_);
    focal_shape_->setScale(Ogre::Vector3(0.05f, 0.05f, 0.01f));
    focal_shape_->setColor(1.0f, 1.0f, 0.0f, 0.5f);
    focal_shape_->getRootNode()->setVisible(false);

    updateWindowSizeProperties();
}

void AnimatedViewController::updateWindowSizeProperties()
{
  window_width_property_->setFloat(context_->getViewManager()->getRenderPanel()->getRenderWindow()->getWidth());
  window_height_property_->setFloat(context_->getViewManager()->getRenderPanel()->getRenderWindow()->getHeight());
}

void AnimatedViewController::onActivate()
{
  updateAttachedSceneNode();

  // Before activation, changes to target frame property should have
  // no side-effects.  After activation, changing target frame
  // property has the side effect (typically) of changing an offset
  // property so that the view does not jump.  Therefore we make the
  // signal/slot connection from the property here in onActivate()
  // instead of in the constructor.
  connect( attached_frame_property_, SIGNAL( changed() ), this, SLOT( updateAttachedFrame() ));
  connect( fixed_up_property_,       SIGNAL( changed() ), this, SLOT( onUpPropertyChanged() ));
  connectPositionProperties();

  // Only do this once activated!
  updateTopics();

}

void AnimatedViewController::connectPositionProperties()
{
  connect( distance_property_,    SIGNAL( changed() ), this, SLOT( onDistancePropertyChanged() ), Qt::UniqueConnection);
  connect( eye_point_property_,   SIGNAL( changed() ), this, SLOT( onEyePropertyChanged() ),      Qt::UniqueConnection);
  connect( focus_point_property_, SIGNAL( changed() ), this, SLOT( onFocusPropertyChanged() ),    Qt::UniqueConnection);
  connect( up_vector_property_,   SIGNAL( changed() ), this, SLOT( onUpPropertyChanged() ),       Qt::UniqueConnection);
}

void AnimatedViewController::disconnectPositionProperties()
{
  disconnect( distance_property_,    SIGNAL( changed() ), this, SLOT( onDistancePropertyChanged() ));
  disconnect( eye_point_property_,   SIGNAL( changed() ), this, SLOT( onEyePropertyChanged() ));
  disconnect( focus_point_property_, SIGNAL( changed() ), this, SLOT( onFocusPropertyChanged() ));
  disconnect( up_vector_property_,   SIGNAL( changed() ), this, SLOT( onUpPropertyChanged() ));
}

void AnimatedViewController::onEyePropertyChanged()
{
  distance_property_->setFloat(getDistanceFromCameraToFocalPoint());
}

void AnimatedViewController::onFocusPropertyChanged()
{
  distance_property_->setFloat(getDistanceFromCameraToFocalPoint());
}

void AnimatedViewController::onDistancePropertyChanged()
{
  disconnectPositionProperties();
  Ogre::Vector3 new_eye_position = focus_point_property_->getVector() + distance_property_->getFloat()* camera_->getOrientation().zAxis();
  eye_point_property_->setVector(new_eye_position);
  connectPositionProperties();
}

void AnimatedViewController::onUpPropertyChanged()
{
  disconnect( up_vector_property_,   SIGNAL( changed() ), this, SLOT( onUpPropertyChanged() ));
  if(fixed_up_property_->getBool()){
    up_vector_property_->setVector(Ogre::Vector3::UNIT_Z);
    camera_->setFixedYawAxis(true, reference_orientation_ * Ogre::Vector3::UNIT_Z);
  }
  else {
    // force orientation to match up vector; first call doesn't actually change the quaternion
    camera_->setFixedYawAxis(true, reference_orientation_ * up_vector_property_->getVector());
    camera_->setDirection( reference_orientation_ * (focus_point_property_->getVector() - eye_point_property_->getVector()));
    // restore normal behavior
    camera_->setFixedYawAxis(false);
  }
  connect( up_vector_property_,   SIGNAL( changed() ), this, SLOT( onUpPropertyChanged() ),       Qt::UniqueConnection);
}

void AnimatedViewController::updateAttachedFrame()
{
  Ogre::Vector3 old_position = attached_scene_node_->getPosition();
  Ogre::Quaternion old_orientation = attached_scene_node_->getOrientation();

  updateAttachedSceneNode();

  onAttachedFrameChanged( old_position, old_orientation );
}

void AnimatedViewController::updateAttachedSceneNode()
{
  Ogre::Vector3 new_reference_position;
  Ogre::Quaternion new_reference_orientation;

  bool queue = false;
  if( context_->getFrameManager()->getTransform( attached_frame_property_->getFrameStd(), ros::Time(),
                                                 new_reference_position, new_reference_orientation ))
  {
    attached_scene_node_->setPosition( new_reference_position );
    attached_scene_node_->setOrientation( new_reference_orientation );
    reference_position_ = new_reference_position;
    reference_orientation_ = new_reference_orientation;
    queue = true;
  }
  if(queue) context_->queueRender();
}

void AnimatedViewController::onAttachedFrameChanged(const Ogre::Vector3& old_reference_position, const Ogre::Quaternion& old_reference_orientation)
{
  Ogre::Vector3 fixed_frame_focus_position = old_reference_orientation*focus_point_property_->getVector() + old_reference_position;
  Ogre::Vector3 fixed_frame_eye_position = old_reference_orientation*eye_point_property_->getVector() + old_reference_position;
  Ogre::Vector3 new_focus_position = fixedFrameToAttachedLocal(fixed_frame_focus_position);
  Ogre::Vector3 new_eye_position = fixedFrameToAttachedLocal(fixed_frame_eye_position);
  Ogre::Vector3 new_up_vector = reference_orientation_.Inverse()*old_reference_orientation*up_vector_property_->getVector();

  //Ogre::Quaternion new_camera_orientation = reference_orientation_.Inverse()*old_reference_orientation*getOrientation();

  focus_point_property_->setVector(new_focus_position);
  eye_point_property_->setVector(new_eye_position);
  up_vector_property_->setVector(fixed_up_property_->getBool() ? Ogre::Vector3::UNIT_Z : new_up_vector);
  distance_property_->setFloat( getDistanceFromCameraToFocalPoint());

  // force orientation to match up vector; first call doesn't actually change the quaternion
  camera_->setFixedYawAxis(true, reference_orientation_ * up_vector_property_->getVector());
  camera_->setDirection( reference_orientation_ * (focus_point_property_->getVector() - eye_point_property_->getVector()));
}

float AnimatedViewController::getDistanceFromCameraToFocalPoint()
{
    return (eye_point_property_->getVector() - focus_point_property_->getVector()).length();
}

void AnimatedViewController::reset()
{
    eye_point_property_->setVector(Ogre::Vector3(5, 5, 10));
    focus_point_property_->setVector(Ogre::Vector3::ZERO);
    up_vector_property_->setVector(Ogre::Vector3::UNIT_Z);
    distance_property_->setFloat( getDistanceFromCameraToFocalPoint());
    mouse_enabled_property_->setBool(true);
    interaction_mode_property_->setStdString(MODE_ORBIT);


  // Hersh says: why is the following junk necessary?  I don't know.
  // However, without this you need to call reset() twice after
  // switching from TopDownOrtho to FPS.  After the first call the
  // camera is in the right position but pointing the wrong way.
  updateCamera();
  camera_->lookAt( 0, 0, 0 );
  setPropertiesFromCamera( camera_ );
}

void AnimatedViewController::handleMouseEvent(ViewportMouseEvent& event)
{
  if( !mouse_enabled_property_->getBool() )
  {
    setCursor( interaction_disabled_cursor_ );
    setStatus( "<b>Mouse interaction is disabled. You can enable it by checking the \"Mouse Enabled\" check-box in the Views panel." );
    return;
  }
  else if ( event.shift() )
  {
    setStatus( "TODO: Fix me! <b>Left-Click:</b> Move X/Y.  <b>Right-Click:</b>: Move Z." );
  }
  else if ( event.control() )
  {
    setStatus( "TODO: Fix me! <b>Left-Click:</b> Move X/Y.  <b>Right-Click:</b>: Move Z." );
  }
  else
  {
    setStatus( "TODO: Fix me! <b>Left-Click:</b> Rotate.  <b>Middle-Click:</b> Move X/Y.  <b>Right-Click:</b>: Zoom.  <b>Shift</b>: More options." );
  }

  float distance = distance_property_->getFloat();
  int32_t diff_x = 0;
  int32_t diff_y = 0;
  bool moved = false;

  if( event.type == QEvent::MouseButtonPress )
  {
    focal_shape_->getRootNode()->setVisible(true);
    moved = true;
    dragging_ = true;
    cancelTransition();  // Stop any automated movement
  }
  else if( event.type == QEvent::MouseButtonRelease )
  {
    focal_shape_->getRootNode()->setVisible(false);
    moved = true;
    dragging_ = false;
  }
  else if( dragging_ && event.type == QEvent::MouseMove )
  {
    diff_x = event.x - event.last_x;
    diff_y = event.y - event.last_y;
    moved = true;
  }

  // regular left-button drag
  if( event.left() && !event.shift() )
  {
    setCursor( Rotate3D );
    yaw_pitch_roll( -diff_x*0.005, -diff_y*0.005, 0 );
  }
  // middle or shift-left drag
  else if( event.middle() || ( event.shift() && event.left() ))
  {
    setCursor( MoveXY );
    if(interaction_mode_property_->getStdString() == MODE_ORBIT)  // Orbit style
    {
        float fovY = camera_->getFOVy().valueRadians();
        float fovX = 2.0f * atan( tan( fovY / 2.0f ) * camera_->getAspectRatio() );

        int width = camera_->getViewport()->getActualWidth();
        int height = camera_->getViewport()->getActualHeight();

        move_focus_and_eye( -((float)diff_x / (float)width) * distance * tan( fovX / 2.0f ) * 2.0f,
              ((float)diff_y / (float)height) * distance * tan( fovY / 2.0f ) * 2.0f,
              0.0f );
    }
    else if(interaction_mode_property_->getStdString() == MODE_FPS)  // Orbit style
    {
      move_focus_and_eye( diff_x*0.01, -diff_y*0.01, 0.0f );
    }
  }
  else if( event.right() )
  {
    if( event.shift() ||  (interaction_mode_property_->getStdString() == MODE_FPS) )
    {
      setCursor( MoveZ );
      move_focus_and_eye(0.0f, 0.0f, diff_y * 0.01 * distance);
    }
    else
    {
      setCursor( Zoom );
      move_eye( 0, 0, diff_y * 0.01 * distance );
    }
  }
  else
  {
    setCursor( event.shift() ? MoveXY : Rotate3D );
  }

  if ( event.wheel_delta != 0 )
  {
    int diff = event.wheel_delta;

    if( event.shift() )
    {
      move_focus_and_eye(0, 0, -diff * 0.001 * distance );
    }
    else if(event.control())
    {
      yaw_pitch_roll(0, 0, diff*0.001 );
    }
    else
    {
      move_eye( 0, 0, -diff * 0.001 * distance );
    }
    moved = true;
  }

  if(event.type == QEvent::MouseButtonPress && event.left() && event.control() && event.shift())
  {
    bool was_orbit = (interaction_mode_property_->getStdString() == MODE_ORBIT);
    interaction_mode_property_->setStdString(was_orbit ? MODE_FPS : MODE_ORBIT );
  }

  if (moved)
  {
    publishCameraPose();
    context_->queueRender();
  }
}

void AnimatedViewController::publishCameraPose()
{
  ros::Time current_time = ros::Time::now();
  Ogre::Vector3 camera_position = eye_point_property_->getVector();

  // on RViz's camera orientation, +z axis points in the focus-to-eye vector's direction, but in Gazebo
  // the camera's +x axis is expected to point towards the focus, hence the 0, pi/2, pi/2 rotation
  tf::Quaternion invQ_tf = tf::createQuaternionFromRPY(0.0, M_PI_2, M_PI_2);
  Ogre::Quaternion invQ(invQ_tf.w(), invQ_tf.x(), invQ_tf.y(), invQ_tf.z());
  Ogre::Quaternion camera_orientation = getOrientation() * invQ;
  camera_orientation.normalise();
  //  ROS_INFO("eye position is x: %f y: %f z: %f", camera_target.x - camera_position.x, camera_target.y - camera_position.y, camera_target.z - camera_position.z);
  //  ROS_INFO("eye orientation is x: %f y: %f z: %f w: %f", camera_orientation.x, camera_orientation.y, camera_orientation.z, camera_orientation.w);
  geometry_msgs::PoseStamped camera_view;
  camera_view.header.stamp = current_time;
  camera_view.header.frame_id = attached_frame_property_->getFrameStd();
  camera_view.pose.position.x = camera_position.x;
  camera_view.pose.position.y = camera_position.y;
  camera_view.pose.position.z = camera_position.z;
  camera_view.pose.orientation.x = camera_orientation.x;
  camera_view.pose.orientation.y = camera_orientation.y;
  camera_view.pose.orientation.z = camera_orientation.z;
  camera_view.pose.orientation.w = camera_orientation.w;
  current_camera_pose_publisher_.publish(camera_view);
}

//void AnimatedViewController::setUpVectorPropertyModeDependent( const Ogre::Vector3 &vector )
//{
//  if(fixed_up_property_->getBool())
//  {
//    //up_vector_property_->setVector(Ogre::Vector3::UNIT_Z);
//  }
//  else {
//    up_vector_property_->setVector(vector);
//  }
//}


void AnimatedViewController::setPropertiesFromCamera( Ogre::Camera* source_camera )
{
  disconnectPositionProperties();
  Ogre::Vector3 direction = source_camera->getOrientation() * Ogre::Vector3::NEGATIVE_UNIT_Z;
  eye_point_property_->setVector( source_camera->getPosition() );
  focus_point_property_->setVector( source_camera->getPosition() + direction*distance_property_->getFloat());
  if(fixed_up_property_->getBool())
    up_vector_property_->setVector(Ogre::Vector3::UNIT_Z);
  else
    up_vector_property_->setVector(source_camera->getOrientation().yAxis());

  //setUpVectorPropertyModeDependent(source_camera->getOrientation().yAxis());
  connectPositionProperties();
}

void AnimatedViewController::mimic( ViewController* source_view )
{
    QVariant target_frame = source_view->subProp( "Target Frame" )->getValue();
    if( target_frame.isValid() )
    {
      attached_frame_property_->setValue( target_frame );
    }

    Ogre::Camera* source_camera = source_view->getCamera();
    Ogre::Vector3 position = source_camera->getPosition();
    Ogre::Quaternion orientation = source_camera->getOrientation();

    if( source_view->getClassId() == "rviz/Orbit" )
    {
        distance_property_->setFloat( source_view->subProp( "Distance" )->getValue().toFloat() );
    }
    else
    {
        distance_property_->setFloat( position.length() );
    }
    interaction_mode_property_->setStdString( MODE_ORBIT );

    Ogre::Vector3 direction = orientation * (Ogre::Vector3::NEGATIVE_UNIT_Z * distance_property_->getFloat() );
    focus_point_property_->setVector( position + direction );
    eye_point_property_->setVector(position);
    updateCamera();
}

void AnimatedViewController::transitionFrom( ViewController* previous_view )
{
  AnimatedViewController* fvc = dynamic_cast<AnimatedViewController*>(previous_view);
  if(fvc)
  {
    Ogre::Vector3 new_eye =   eye_point_property_->getVector();
    Ogre::Vector3 new_focus = focus_point_property_->getVector();
    Ogre::Vector3 new_up =    up_vector_property_->getVector();

    eye_point_property_->setVector(fvc->eye_point_property_->getVector());
    focus_point_property_->setVector(fvc->focus_point_property_->getVector());
    up_vector_property_->setVector(fvc->up_vector_property_->getVector());

    beginNewTransition(new_eye, new_focus, new_up, ros::Duration(default_transition_time_property_->getFloat()));
  }
}

void AnimatedViewController::beginNewTransition(const Ogre::Vector3 &eye, 
                                                const Ogre::Vector3 &focus, 
                                                const Ogre::Vector3 &up,
                                                ros::Duration transition_duration,
                                                uint8_t interpolation_speed)
{
  if(transition_duration.toSec() < 0.0)
    return;

  // convert positional jumps to very fast movements to prevent numerical problems 
  if(transition_duration.isZero())
    transition_duration = ros::Duration(0.001);

  // if the buffer is empty we set the first element in it to the current camera pose
  if(cam_movements_buffer_.empty())
  {
    transition_start_time_ = ros::WallTime::now();

    cam_movements_buffer_.push_back(std::move(OgreCameraMovement(eye_point_property_->getVector(),
                                                                 focus_point_property_->getVector(),
                                                                 up_vector_property_->getVector(),
                                                                 ros::Duration(0.001),
                                                                 interpolation_speed))); // interpolation_speed doesn't make a difference for very short times
  }

  if(cam_movements_buffer_.full())
    cam_movements_buffer_.set_capacity(cam_movements_buffer_.capacity() + 20);

  cam_movements_buffer_.push_back(std::move(OgreCameraMovement(eye, focus, up, transition_duration, interpolation_speed)));

  animate_ = true;
}

void AnimatedViewController::cancelTransition()
{
  animate_ = false;

  cam_movements_buffer_.clear();
  rendered_frames_counter_ = 0;

  if(render_frame_by_frame_)
  {
    std_msgs::Bool finished_animation;
    finished_animation.data = 1;  // set to true, but std_msgs::Bool is uint8 internally
    finished_animation_publisher_.publish(finished_animation);
    render_frame_by_frame_ = false;
  }
}

void AnimatedViewController::cameraPlacementCallback(const CameraPlacementConstPtr &cp_ptr)
{
  CameraPlacement cp = *cp_ptr;

  // Handle control parameters
  mouse_enabled_property_->setBool( !cp.interaction_disabled );
  fixed_up_property_->setBool( !cp.allow_free_yaw_axis );
  if(cp.mouse_interaction_mode != cp.NO_CHANGE)
  {
    std::string name = "";
    if(cp.mouse_interaction_mode == cp.ORBIT) name = MODE_ORBIT;
    else if(cp.mouse_interaction_mode == cp.FPS) name = MODE_FPS;
    interaction_mode_property_->setStdString(name);
  }

  if(cp.target_frame != "")
  {
    attached_frame_property_->setStdString(cp.target_frame);
    updateAttachedFrame();
  }

  if(cp.time_from_start.toSec() >= 0)
  {
    ROS_DEBUG_STREAM("Received a camera placement request! \n" << cp);
    transformCameraToAttachedFrame(cp.eye,
                                   cp.focus,
                                   cp.up);
    ROS_DEBUG_STREAM("After transform, we have \n" << cp);

    Ogre::Vector3 eye = vectorFromMsg(cp.eye.point);
    Ogre::Vector3 focus = vectorFromMsg(cp.focus.point);
    Ogre::Vector3 up = vectorFromMsg(cp.up.vector);

    beginNewTransition(eye, focus, up, cp.time_from_start);
  }
}

void AnimatedViewController::cameraTrajectoryCallback(const view_controller_msgs::CameraTrajectoryConstPtr& ct_ptr)
{
  view_controller_msgs::CameraTrajectory ct = *ct_ptr;

  if(ct.trajectory.empty())
    return;

  // Handle control parameters
  mouse_enabled_property_->setBool(!ct.interaction_disabled);
  fixed_up_property_->setBool(!ct.allow_free_yaw_axis);
  if(ct.mouse_interaction_mode != view_controller_msgs::CameraTrajectory::NO_CHANGE)
  {
    std::string name = "";
    if(ct.mouse_interaction_mode == view_controller_msgs::CameraTrajectory::ORBIT)
      name = MODE_ORBIT;
    else if(ct.mouse_interaction_mode == view_controller_msgs::CameraTrajectory::FPS)
      name = MODE_FPS;
    interaction_mode_property_->setStdString(name);
  }

  if(ct.render_frame_by_frame > 0)
  {
    render_frame_by_frame_ = true;
    target_fps_ = static_cast<int>(ct.frames_per_second);
    publish_view_images_property_->setBool(true);
  }

  for(auto& cam_movement : ct.trajectory)
  {
    if(cam_movement.transition_duration.toSec() >= 0.0)
    {
      if(ct.target_frame != "")
      {
        attached_frame_property_->setStdString(ct.target_frame);
        updateAttachedFrame();
      }

      transformCameraToAttachedFrame(cam_movement.eye,
                                     cam_movement.focus,
                                     cam_movement.up);

      Ogre::Vector3 eye = vectorFromMsg(cam_movement.eye.point);
      Ogre::Vector3 focus = vectorFromMsg(cam_movement.focus.point);
      Ogre::Vector3 up = vectorFromMsg(cam_movement.up.vector);
      beginNewTransition(eye, focus, up, cam_movement.transition_duration, cam_movement.interpolation_speed);
    }
    else
    {
      ROS_WARN("Transition duration of camera movement is below zero. Skipping that movement.");
    }
  }
}

void AnimatedViewController::transformCameraToAttachedFrame(geometry_msgs::PointStamped& eye,
                                                            geometry_msgs::PointStamped& focus,
                                                            geometry_msgs::Vector3Stamped& up)
{
  Ogre::Vector3 position_fixed_eye, position_fixed_focus, position_fixed_up;
  Ogre::Quaternion rotation_fixed_eye, rotation_fixed_focus, rotation_fixed_up;

  context_->getFrameManager()->getTransform(eye.header.frame_id,   ros::Time(0), position_fixed_eye,   rotation_fixed_eye);
  context_->getFrameManager()->getTransform(focus.header.frame_id, ros::Time(0), position_fixed_focus, rotation_fixed_focus);
  context_->getFrameManager()->getTransform(up.header.frame_id,    ros::Time(0), position_fixed_up,    rotation_fixed_up);

  Ogre::Vector3 ogre_eye = vectorFromMsg(eye.point);
  Ogre::Vector3 ogre_focus = vectorFromMsg(focus.point);
  Ogre::Vector3 ogre_up = vectorFromMsg(up.vector);

  ogre_eye = fixedFrameToAttachedLocal(position_fixed_eye + rotation_fixed_eye * ogre_eye);
  ogre_focus = fixedFrameToAttachedLocal(position_fixed_focus + rotation_fixed_focus * ogre_focus);
  ogre_up = reference_orientation_.Inverse() * rotation_fixed_up * ogre_up;

  eye.point = pointOgreToMsg(ogre_eye);
  focus.point = pointOgreToMsg(ogre_focus);
  up.vector = vectorOgreToMsg(ogre_up);
  eye.header.frame_id = attached_frame_property_->getStdString();
  focus.header.frame_id = attached_frame_property_->getStdString();
  up.header.frame_id = attached_frame_property_->getStdString();
}

// We must assume that this point is in the Rviz Fixed frame since it came from Rviz...
void AnimatedViewController::lookAt( const Ogre::Vector3& point )
{
  if( !mouse_enabled_property_->getBool() ) return;

  Ogre::Vector3 new_point = fixedFrameToAttachedLocal(point);

  beginNewTransition(eye_point_property_->getVector(), new_point,
                     up_vector_property_->getVector(),
                     ros::Duration(default_transition_time_property_->getFloat()));

  //  // Just for easily testing the other movement styles:
  //  orbitCameraTo(point);
  //  moveCameraWithFocusTo(point);
}

void AnimatedViewController::orbitCameraTo( const Ogre::Vector3& point)
{
  beginNewTransition(point, focus_point_property_->getVector(),
                     up_vector_property_->getVector(),
                     ros::Duration(default_transition_time_property_->getFloat()));
}

void AnimatedViewController::moveEyeWithFocusTo( const Ogre::Vector3& point)
{
  beginNewTransition(point, focus_point_property_->getVector() + (point - eye_point_property_->getVector()),
                     up_vector_property_->getVector(),
                     ros::Duration(default_transition_time_property_->getFloat()));
}


void AnimatedViewController::update(float dt, float ros_dt)
{
  updateAttachedSceneNode();

  if(animate_ && isMovementAvailable())
  {
    pauseAnimationOnRequest();

    auto start = cam_movements_buffer_.begin();
    auto goal = ++(cam_movements_buffer_.begin());

    double relative_progress_in_time = computeRelativeProgressInTime(goal->transition_duration);

    // make sure we get all the way there before turning off
    bool finished_current_movement = false;
    if(relative_progress_in_time >= 1.0)
    {
      relative_progress_in_time = 1.0;
      finished_current_movement = true;
    }

    float relative_progress_in_space = computeRelativeProgressInSpace(relative_progress_in_time,
                                                                      goal->interpolation_speed);

    Ogre::Vector3 new_position = start->eye + relative_progress_in_space * (goal->eye - start->eye);
    Ogre::Vector3 new_focus = start->focus + relative_progress_in_space * (goal->focus - start->focus);
    Ogre::Vector3 new_up = start->up + relative_progress_in_space * (goal->up - start->up);

    disconnectPositionProperties();
    eye_point_property_->setVector( new_position );
    focus_point_property_->setVector( new_focus );
    up_vector_property_->setVector(new_up);
    distance_property_->setFloat( getDistanceFromCameraToFocalPoint());
    connectPositionProperties();

    // This needs to happen so that the camera orientation will update properly when fixed_up_property == false
    camera_->setFixedYawAxis(true, reference_orientation_ * up_vector_property_->getVector());
    camera_->setDirection(reference_orientation_ * (focus_point_property_->getVector() - eye_point_property_->getVector()));

    publishCameraPose();
    
    if(publish_view_images_property_->getBool())
      publishViewImage();

    if(finished_current_movement)
    {
      // delete current start element in buffer
      cam_movements_buffer_.pop_front();

      if(isMovementAvailable())
        prepareNextMovement(goal->transition_duration);
      else
        cancelTransition();
    }
  }
  updateCamera();
  updateWindowSizeProperties();
}

void AnimatedViewController::pauseAnimationOnRequest()
{
  if(pause_animation_duration_.toSec() > 0.0)
  {
    pause_animation_duration_.sleep();
    transition_start_time_ += pause_animation_duration_;
    pause_animation_duration_.fromSec(0.0);
  }
}

double AnimatedViewController::computeRelativeProgressInTime(const ros::Duration& transition_duration)
{
  double relative_progress_in_time = 0.0;
  if(render_frame_by_frame_)
  {
    relative_progress_in_time = rendered_frames_counter_ / (target_fps_ * transition_duration.toSec());
    rendered_frames_counter_++;
  }
  else
  {
    ros::WallDuration duration_from_start = ros::WallTime::now() - transition_start_time_;
    relative_progress_in_time = duration_from_start.toSec() / transition_duration.toSec();
  }
  return relative_progress_in_time;
}

float AnimatedViewController::computeRelativeProgressInSpace(double relative_progress_in_time,
                                                             uint8_t interpolation_speed)
{
  switch(interpolation_speed)
  {
    case view_controller_msgs::CameraMovement::RISING:
      return 1.f - static_cast<float>(cos(relative_progress_in_time * M_PI_2));
    case view_controller_msgs::CameraMovement::DECLINING:
      return static_cast<float>(-cos(relative_progress_in_time * M_PI_2 + M_PI_2));
    case view_controller_msgs::CameraMovement::FULL:
      return static_cast<float>(relative_progress_in_time);
    case view_controller_msgs::CameraMovement::WAVE:
    default:
      return 0.5f * (1.f - static_cast<float>(cos(relative_progress_in_time * M_PI)));
  }
}

void AnimatedViewController::publishViewImage()
{
  if(camera_view_image_publisher_.getNumSubscribers() > 0)
  {
    std::shared_ptr<Ogre::PixelBox> pixel_box = std::make_shared<Ogre::PixelBox>();
    getViewImage(pixel_box);

    sensor_msgs::ImagePtr image_msg = sensor_msgs::ImagePtr(new sensor_msgs::Image());
    convertImage(pixel_box, image_msg);

    camera_view_image_publisher_.publish(image_msg);

    delete[] (unsigned char*)pixel_box->data;
  }
}

void AnimatedViewController::getViewImage(std::shared_ptr<Ogre::PixelBox>& pixel_box)
{
  const unsigned int image_height = context_->getViewManager()->getRenderPanel()->getRenderWindow()->getHeight();
  const unsigned int image_width = context_->getViewManager()->getRenderPanel()->getRenderWindow()->getWidth();

  // create a PixelBox to store the rendered view image
  const Ogre::PixelFormat pixel_format = Ogre::PF_BYTE_BGR;
  const auto bytes_per_pixel = Ogre::PixelUtil::getNumElemBytes(pixel_format);
  auto image_data = new unsigned char[image_width * image_height * bytes_per_pixel];
  Ogre::Box image_extents(0, 0, image_width, image_height);
  pixel_box = std::make_shared<Ogre::PixelBox>(image_extents, pixel_format, image_data);
  context_->getViewManager()->getRenderPanel()->getRenderWindow()->copyContentsToMemory(*pixel_box,
                                                                                        Ogre::RenderTarget::FB_AUTO);
}

void AnimatedViewController::convertImage(std::shared_ptr<Ogre::PixelBox> input_image,
                                          sensor_msgs::ImagePtr output_image)
{
  const auto bytes_per_pixel = Ogre::PixelUtil::getNumElemBytes(input_image->format);
  const auto image_height = input_image->getHeight();
  const auto image_width = input_image->getWidth();

  output_image->header.frame_id = attached_frame_property_->getStdString();
  output_image->header.stamp = ros::Time::now();
  output_image->height = image_height;
  output_image->width = image_width;
  output_image->encoding = sensor_msgs::image_encodings::BGR8;
  output_image->is_bigendian = false;
  output_image->step = static_cast<unsigned int>(image_width * bytes_per_pixel);
  size_t size = image_width * image_height * bytes_per_pixel;
  output_image->data.resize(size);
  memcpy((char*)(&output_image->data[0]), input_image->data, size);
}

void AnimatedViewController::prepareNextMovement(const ros::Duration& previous_transition_duration)
{
  transition_start_time_ += ros::WallDuration(previous_transition_duration.toSec());
  rendered_frames_counter_ = 0;
}

void AnimatedViewController::updateCamera()
{
  camera_->setPosition( eye_point_property_->getVector() );
  camera_->setFixedYawAxis(fixed_up_property_->getBool(), reference_orientation_ * up_vector_property_->getVector());
  camera_->setDirection( reference_orientation_ * (focus_point_property_->getVector() - eye_point_property_->getVector()));
  //camera_->setDirection( (focus_point_property_->getVector() - eye_point_property_->getVector()));
  focal_shape_->setPosition( focus_point_property_->getVector() );
}

void AnimatedViewController::yaw_pitch_roll( float yaw, float pitch, float roll )
{
  Ogre::Quaternion old_camera_orientation = camera_->getOrientation();
  Ogre::Radian old_pitch = old_camera_orientation.getPitch(false);// - Ogre::Radian(Ogre::Math::HALF_PI);

  if(fixed_up_property_->getBool()) yaw = cos(old_pitch.valueRadians() - Ogre::Math::HALF_PI)*yaw; // helps to reduce crazy spinning!

  Ogre::Quaternion yaw_quat, pitch_quat, roll_quat;
  yaw_quat.FromAngleAxis( Ogre::Radian( yaw ), Ogre::Vector3::UNIT_Y );
  pitch_quat.FromAngleAxis( Ogre::Radian( pitch ), Ogre::Vector3::UNIT_X );
  roll_quat.FromAngleAxis( Ogre::Radian( roll ), Ogre::Vector3::UNIT_Z );
  Ogre::Quaternion orientation_change = yaw_quat * pitch_quat * roll_quat;
  Ogre::Quaternion new_camera_orientation = old_camera_orientation * orientation_change;
  Ogre::Radian new_pitch = new_camera_orientation.getPitch(false);// - Ogre::Radian(Ogre::Math::HALF_PI);

  if( fixed_up_property_->getBool() &&
      ((new_pitch > PITCH_LIMIT_HIGH && new_pitch > old_pitch) || (new_pitch < PITCH_LIMIT_LOW && new_pitch < old_pitch)) )
  {
    orientation_change = yaw_quat * roll_quat;
    new_camera_orientation = old_camera_orientation * orientation_change;
  }

//  Ogre::Radian new_roll = new_camera_orientation.getRoll(false);
//  Ogre::Radian new_yaw = new_camera_orientation.getYaw(false);
  //ROS_INFO("old_pitch: %.3f, new_pitch: %.3f", old_pitch.valueRadians(), new_pitch.valueRadians());

  camera_->setOrientation( new_camera_orientation );
  if( interaction_mode_property_->getStdString() == MODE_ORBIT )
  {
    // In orbit mode the focal point stays fixed, so we need to compute the new camera position.
    Ogre::Vector3 new_eye_position = focus_point_property_->getVector() + distance_property_->getFloat()* new_camera_orientation.zAxis();
    eye_point_property_->setVector(new_eye_position);
    camera_->setPosition(new_eye_position);
    setPropertiesFromCamera(camera_);
  }
  else
  {
    // In FPS mode the camera stays fixed, so we can just apply the rotations and then rely on the property update to set the new focal point.
    setPropertiesFromCamera(camera_);
  }
}

Ogre::Quaternion AnimatedViewController::getOrientation()  // Do we need this?
{
  return camera_->getOrientation();
}

void AnimatedViewController::move_focus_and_eye( float x, float y, float z )
{
  Ogre::Vector3 translate( x, y, z );
  eye_point_property_->add( getOrientation() * translate );
  focus_point_property_->add( getOrientation() * translate );
}

void AnimatedViewController::move_eye( float x, float y, float z )
{
  Ogre::Vector3 translate( x, y, z );
  // Only update the camera position if it won't "pass through" the origin
  Ogre::Vector3 new_position = eye_point_property_->getVector() + getOrientation() * translate;
  if( (new_position - focus_point_property_->getVector()).length() > distance_property_->getMin() )
    eye_point_property_->setVector(new_position);
  distance_property_->setFloat(getDistanceFromCameraToFocalPoint());
}



} // end namespace rviz

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS( rviz_animated_view_controller::AnimatedViewController, rviz::ViewController )
