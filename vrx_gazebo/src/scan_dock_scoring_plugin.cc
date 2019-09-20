/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <algorithm>
#include <cmath>
#include <gazebo/common/Console.hh>
#include "vrx_gazebo/scan_dock_scoring_plugin.hh"

/////////////////////////////////////////////////
ColorSequenceChecker::ColorSequenceChecker(
  const std::vector<std::string> &_expectedColors,
  const std::string &_rosNameSpace, const std::string &_rosColorSequenceService)
  : expectedSequence(_expectedColors),
    ns(_rosNameSpace),
    colorSequenceService(_rosColorSequenceService)
{
  // Quit if ros plugin was not loaded
  if (!ros::isInitialized())
  {
    ROS_ERROR("ROS was not initialized.");
    return;
  }
  this->nh = ros::NodeHandle(this->ns);
}

/////////////////////////////////////////////////
void ColorSequenceChecker::Enable()
{
  this->colorSequenceServer = this->nh.advertiseService(
    this->colorSequenceService, &ColorSequenceChecker::OnColorSequence, this);
}

/////////////////////////////////////////////////
void ColorSequenceChecker::Disable()
{
  this->colorSequenceServer.shutdown();
}

/////////////////////////////////////////////////
bool ColorSequenceChecker::SubmissionReceived() const
{
  return this->colorSequenceReceived;
}

/////////////////////////////////////////////////
bool ColorSequenceChecker::Correct() const
{
  return this->correctSequence;
}

/////////////////////////////////////////////////
bool ColorSequenceChecker::OnColorSequence(
  ros::ServiceEvent<vrx_gazebo::ColorSequence::Request,
    vrx_gazebo::ColorSequence::Response> &_event)
{
  ROS_INFO_NAMED("ColorSequenceChecker", "Color sequence submission received");

  const vrx_gazebo::ColorSequence::Request &req = _event.getRequest();
  vrx_gazebo::ColorSequence::Response &res = _event.getResponse();

  {
    // Sanity check: Only one color sequence submission is allowed.
    if (this->colorSequenceReceived)
    {
      ROS_ERROR("The color sequence has already been submitted");
      res.success = false;
      return false;
    }

    this->colorSequenceReceived = true;
  }

  // Sanity check: Make sure that we have the expected color sequence.
  if (this->expectedSequence.size() != 3u)
  {
    res.success = false;
    return false;
  }

  std::string color1 = req.color1;
  std::string color2 = req.color2;
  std::string color3 = req.color3;

  std::transform(color1.begin(), color1.end(), color1.begin(), ::tolower);
  std::transform(color2.begin(), color2.end(), color2.begin(), ::tolower);
  std::transform(color3.begin(), color3.end(), color3.begin(), ::tolower);

  // Incorrect color sequence.
  this->correctSequence =
      color1 == this->expectedSequence[0] &&
      color2 == this->expectedSequence[1] &&
      color3 == this->expectedSequence[2];

  // The submission is considered correct even if the sequence is wrong.
  res.success = true;
  return true;
}

/////////////////////////////////////////////////
DockChecker::DockChecker(const std::string &_name,
  const std::string &_activationTopic, const double _minDockTime,
  const bool _dockAllowed, const std::string &_worldName,
  const std::string &_rosNameSpace, const std::string &_announceSymbol,
  const std::string &_gzSymbolTopic)
  : name(_name),
    activationTopic(_activationTopic),
    minDockTime(_minDockTime),
    dockAllowed(_dockAllowed),
    ns(_rosNameSpace),
    gzSymbolTopic(_gzSymbolTopic)
{
  this->timer.Stop();
  this->timer.Reset();

  this->announceSymbol.data = _announceSymbol;

  // Subscriber to receive world updates (e.g.: a notification after a cloning).
  this->node.reset(new gazebo::transport::Node());
  this->node->Init();

  this->containSub = this->node->Subscribe(this->activationTopic,
    &DockChecker::OnActivationEvent, this);
}

/////////////////////////////////////////////////
bool DockChecker::AnytimeDocked() const
{
  return this->anytimeDocked;
}

/////////////////////////////////////////////////
bool DockChecker::CurrentlyDocked() const
{
  return this->currentlyDocked;
}

/////////////////////////////////////////////////
bool DockChecker::Allowed() const
{
  return this->dockAllowed;
}

/////////////////////////////////////////////////
void DockChecker::AnnounceSymbol()
{
  // Override the docks own sdf parameters
  this->dockPlacardPub = this->node->Advertise
    <dock_placard_msgs::msgs::DockPlacard>(gzSymbolTopic);
  dock_placard_msgs::msgs::DockPlacard symbol;
  symbol.set_color(announceSymbol.data.substr
    (0, announceSymbol.data.find("_")));
  symbol.set_shape(announceSymbol.data.substr
    (announceSymbol.data.find("_")+1));
  this->dockPlacardPub->Publish(symbol);

  if (this->dockAllowed)
  {
    // Initialize ROS transport.
    this->nh.reset(new ros::NodeHandle());
    this->symbolPub =
      this->nh->advertise<std_msgs::String>(this->symbolTopic, 1, true);

    this->symbolPub.publish(this->announceSymbol);
  }
}

/////////////////////////////////////////////////
void DockChecker::Update()
{
  if (this->anytimeDocked)
    return;

  this->anytimeDocked =
    this->timer.GetElapsed() >= gazebo::common::Time(this->minDockTime);
}

/////////////////////////////////////////////////
void DockChecker::OnActivationEvent(ConstIntPtr &_msg)
{
  this->currentlyDocked = _msg->data() == 1;

  if (_msg->data() == 1)
    this->timer.Start();

  if (_msg->data() == 0)
  {
    this->timer.Stop();
    this->timer.Reset();
  }

  gzdbg << "[" << this->name << "] OnActivationEvent(): "
        << _msg->data() << std::endl;
}

//////////////////////////////////////////////////
ScanDockScoringPlugin::ScanDockScoringPlugin():
  node (new gazebo::transport::Node())
{
}

/////////////////////////////////////////////////
void ScanDockScoringPlugin::Load(gazebo::physics::WorldPtr _world,
    sdf::ElementPtr _sdf)
{
  this->node->Init();
  ScoringPlugin::Load(_world, _sdf);

  gzmsg << "Task [" << this->TaskName() << "]" << std::endl;

  if (!this->ParseSDF(_sdf))
    return;

  this->updateConnection = gazebo::event::Events::ConnectWorldUpdateBegin(
    std::bind(&ScanDockScoringPlugin::Update, this));

  this->lightBuoySequencePub = this->node->Advertise
    <light_buoy_colors_msgs::msgs::LightBuoyColors>(this->colorTopic);
}

//////////////////////////////////////////////////
bool ScanDockScoringPlugin::ParseSDF(sdf::ElementPtr _sdf)
{
  // Optional: ROS namespace.
  std::string ns;
  if (_sdf->HasElement("robot_namespace"))
    ns = _sdf->GetElement("robot_namespace")->Get<std::string>();

  // Optional: ROS service.
  std::string colorSequenceService = "/vrx/scan_dock/color_sequence";
  if (_sdf->HasElement("color_sequence_service"))
  {
    colorSequenceService =
      _sdf->GetElement("color_sequence_service")->Get<std::string>();
  }

  // Required: The expected color pattern.
  for (auto colorIndex : {"color_1", "color_2", "color_3"})
  {
    if (!_sdf->HasElement(colorIndex))
    {
      ROS_ERROR("<%s> missing", colorIndex);
      return false;
    }

    auto color = _sdf->GetElement(colorIndex)->Get<std::string>();
    std::transform(color.begin(), color.end(), color.begin(), ::tolower);

    // Sanity check: color should be red, green, blue or yellow.
    if (color != "red"  && color != "green" &&
        color != "blue" && color != "yellow")
    {
      ROS_ERROR("Invalid color [%s]", color.c_str());
      return false;
    }
    this->expectedSequence.push_back(color);
  }

  // Optional: gazebo topic where light buoy sequence is published
  if (!_sdf->HasElement("color_topic"))
  {
    this->colorTopic = "/vrx/light_buoy/new_pattern";
  }
  else
  {
    this->colorTopic = _sdf->GetElement("color_topic")->Get<std::string>();
  }

  // Optional: the points granted when reported the correct color sequence.
  if (_sdf->HasElement("color_bonus_points"))
  {
    this->colorBonusPoints =
      _sdf->GetElement("color_bonus_points")->Get<double>();
  }

  // Instantiate the color checker.
  this->colorChecker.reset(
    new ColorSequenceChecker(this->expectedSequence, ns, colorSequenceService));

  // Required: Parse the bays.
  if (!_sdf->HasElement("bays"))
  {
    ROS_ERROR("<bays> missing");
    return false;
  }

  auto baysElem = _sdf->GetElement("bays");
  if (!baysElem->HasElement("bay"))
  {
    ROS_ERROR("<bay> missing");
    return false;
  }

  auto bayElem = baysElem->GetElement("bay");
  while (bayElem)
  {
    // Required: bay name.
    if (!bayElem->GetElement("name"))
    {
      ROS_ERROR("<gates::gate::name> missing");
      return false;
    }
    std::string bayName = bayElem->Get<std::string>("name");

    // Required: activation topic.
    if (!bayElem->GetElement("activation_topic"))
    {
      ROS_ERROR("<gates::gate::activation_topic> missing");
      return false;
    }
    std::string activationTopic = bayElem->Get<std::string>("activation_topic");

    // Required: gazebo symbol topic.
    if (!bayElem->GetElement("symbol_topic"))
    {
      ROS_ERROR("<gates::gate::symbol_topic> missing");
      return false;
    }
    std::string symbolTopic = bayElem->Get<std::string>("symbol_topic");

    // Required: minimum time to be considered "docked".
    if (!bayElem->GetElement("min_dock_time"))
    {
      ROS_ERROR("<gates::gate::min_dock_time> missing");
      return false;
    }
    double minDockTime = bayElem->Get<double>("min_dock_time");

    // Required: dock allowed.
    if (!bayElem->GetElement("dock_allowed"))
    {
      ROS_ERROR("<gates::gate::dock_allowed> missing");
      return false;
    }
    bool dockAllowed = bayElem->Get<bool>("dock_allowed");

    std::string announceSymbol = "";
    if (!bayElem->HasElement("symbol"))
    {
      ROS_ERROR("<symbol> not found");
    }
    announceSymbol =
      bayElem->GetElement("symbol")->Get<std::string>();


    // Create a new dock checker.
    #if GAZEBO_MAJOR_VERSION >= 8
      std::unique_ptr<DockChecker> dockChecker(
        new DockChecker(bayName, activationTopic, minDockTime, dockAllowed,
          this->world->Name(), ns, announceSymbol, symbolTopic));
    #else
      std::unique_ptr<DockChecker> dockChecker(
        new DockChecker(bayName, activationTopic, minDockTime, dockAllowed,
          this->world->GetName(), ns, announceSymbol, symbolTopic));
    #endif

    // Add the dock checker.
    this->dockCheckers.push_back(std::move(dockChecker));

    // Process the next checker.
    bayElem = bayElem->GetNextElement();
  }

  // Optional: the points granted when the vehicle docks in any bay.
  if (_sdf->HasElement("dock_bonus_points"))
  {
    this->dockBonusPoints =
      _sdf->GetElement("dock_bonus_points")->Get<double>();
  }

  // Optional: the points granted when the vehicle docks in the expected bay.
  if (_sdf->HasElement("correct_dock_bonus_points"))
  {
    this->correctDockBonusPoints =
      _sdf->GetElement("correct_dock_bonus_points")->Get<double>();
  }

  return true;
}

//////////////////////////////////////////////////
void ScanDockScoringPlugin::Update()
{
  // Verify the color checker.
  if (!this->colorSubmissionProcessed &&
      this->colorChecker->SubmissionReceived())
  {
    // We need to decide if we grant extra points.
    if (this->colorChecker->Correct())
      this->SetScore(this->Score() + this->colorBonusPoints);

    // We only allow one color sequence submission.
    this->colorChecker->Disable();
    this->colorSubmissionProcessed = true;
  }

  // Verify the dock checkers.
  for (auto &dockChecker : this->dockCheckers)
  {
    // We always need to update the checkers.
    dockChecker->Update();

    // Nothing to do if nobody ever docked or we're currently docked.
    if (!dockChecker->AnytimeDocked() || dockChecker->CurrentlyDocked())
      continue;

    // Points granted for docking!
    this->SetScore(this->Score() + this->dockBonusPoints);
    gzmsg << "Successfully docked - granting <" << this->dockBonusPoints 
      << "> points" <<std::endl;  

    // Is this the right bay?
    if (dockChecker->Allowed())
    {
      this->SetScore(this->Score() + this->correctDockBonusPoints);
      gzmsg << "Docked in the correct bay - granting <" 
        << this->correctDockBonusPoints 
        << "> points" <<std::endl;  
    }
    else
    {
        gzmsg << "Incorrect docking bay - granting no poitns" << std::endl;
    }

    // Time to finish the task as the vehicle docked.
    // Note that we only allow to dock one time. This is to prevent teams
    // docking in all possible bays.
    this->Finish();
    break;
  }
}

//////////////////////////////////////////////////
void ScanDockScoringPlugin::OnReady()
{
  gzmsg << "OnReady" << std::endl;
  // Announce the symbol if needed.
  for (auto &dockChecker : this->dockCheckers)
    dockChecker->AnnounceSymbol();
}

//////////////////////////////////////////////////
void ScanDockScoringPlugin::OnRunning()
{
  gzmsg << "OnRunning" << std::endl;
  light_buoy_colors_msgs::msgs::LightBuoyColors colors;
  colors.set_color_1(this->expectedSequence[0]);
  colors.set_color_2(this->expectedSequence[1]);
  colors.set_color_3(this->expectedSequence[2]);
  lightBuoySequencePub->Publish(colors);

  this->colorChecker->Enable();
  // Announce the symbol if needed.
  for (auto &dockChecker : this->dockCheckers)
    dockChecker->AnnounceSymbol();
}

// Register plugin with gazebo
GZ_REGISTER_WORLD_PLUGIN(ScanDockScoringPlugin)
