/*
 * Copyright (c) 2017-9, Ubiquity Robotics
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
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
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 *
 */

#include <fiducial_slam/helpers.h>

#include <assert.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include <ros/ros.h>
#include <tf/transform_datatypes.h>
#include <tf2/LinearMath/Transform.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/transform_listener.h>
#include <visualization_msgs/Marker.h>

#include "fiducial_msgs/Fiducial.h"
#include "fiducial_msgs/FiducialArray.h"
#include "fiducial_msgs/FiducialTransform.h"
#include "fiducial_msgs/FiducialTransformArray.h"

#include "fiducial_slam/map.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/highgui.hpp>

#include <list>
#include <string>

using namespace std;
using namespace cv;

class FiducialSlam {
private:
    ros::Subscriber ft_sub;

    ros::Subscriber verticesSub;
    ros::Subscriber cameraInfoSub;
    ros::Publisher ftPub;

    bool use_fiducial_area_as_weight;
    double weighting_scale;

    void transformCallback(const fiducial_msgs::FiducialTransformArray::ConstPtr &msg);

public:
    bool use_read_only_map;
    bool fiducialsFlat;
    bool verboseInfo;
    Map fiducialMap;
    FiducialSlam(ros::NodeHandle &nh);
};

// To sort the fiducials we need a compare routine looking at fiducial id
static bool compareObservation(Observation obs1, Observation obs2) {
    return (obs1.fid < obs2.fid);
}

// transformCallback gets fiducials currently in view as found by aruco package.
// These fiducials are placed into 'observations' vector and are relative to camera frame
void FiducialSlam::transformCallback(const fiducial_msgs::FiducialTransformArray::ConstPtr &msg) {
    vector<Observation> observations;

    for (size_t i = 0; i < msg->transforms.size(); i++) {
        const fiducial_msgs::FiducialTransform &ft = msg->transforms[i];

        tf2::Vector3 tvec(ft.transform.translation.x, ft.transform.translation.y,
                          ft.transform.translation.z);

        // A special mode of forcing the fiducials to be flat can only be used in
        // environments where ceiling and floor are parallel or both flat to earth.
        // This is basically a workaround to eliminate fiducial noise in roll and pitch
        if (fiducialsFlat) {
            tf2::Quaternion q(0.0, 0.0, ft.transform.rotation.z, ft.transform.rotation.w);
        } else {
            tf2::Quaternion q(ft.transform.rotation.x, ft.transform.rotation.y, ft.transform.rotation.z,
                          ft.transform.rotation.w);
        }

        if (verboseInfo) {
            ROS_INFO("FSlam: fid %d obj_err %9.5lf", ft.fiducial_id, ft.object_error);
        }
        double variance;
        if (use_fiducial_area_as_weight) {
            variance = weighting_scale / ft.fiducial_area;
        } else {
            variance = weighting_scale * ft.object_error;
        }

        Observation obs(ft.fiducial_id, tf2::Stamped<TransformWithVariance>(
                                            TransformWithVariance(ft.transform, variance),
                                            msg->header.stamp, msg->header.frame_id));
        observations.push_back(obs);
    }

    // To make debug easier sort the observations by fiducial id 
    std::sort(observations.begin(), observations.end(), compareObservation); 

    // Walk the sorted fiducial list and show translation from camera (not from base_link)
    if (verboseInfo) {
        for (Observation &o : observations) {
            auto cam_f = o.T_camFid.transform.getOrigin();
            ROS_INFO("FSlam: fid %d  XYZ %9.6lf %9.6lf %9.6lf",
                o.fid, cam_f.x(), cam_f.y(), cam_f.z());
        }
    }

    fiducialMap.update(observations, msg->header.stamp);
}

FiducialSlam::FiducialSlam(ros::NodeHandle &nh) : fiducialMap(nh) {
    bool doPoseEstimation;

    // If set, use the fiducial area in pixels^2 as an indication of the
    // 'goodness' of it. This will favor fiducials that are close to the
    // camera and center of the image. The reciprical of the area is actually
    // used, in place of reprojection error as the estimate's variance
    nh.param<bool>("use_fiducial_area_as_weight", use_fiducial_area_as_weight, false);
    // Scaling factor for weighing
    nh.param<double>("weighting_scale", weighting_scale, 1e9);

    nh.param("do_pose_estimation", doPoseEstimation, false);

    // Forces a nav 2D mode where flat floor is assumed
    nh.param<bool>("fiducials_flat", fiducialsFlat, false); 

    nh.param("read_only_map", use_read_only_map, false);
    if (use_read_only_map) {
        ROS_INFO("Fiducial Slam in READ ONLY MAP MODE!");
    } else {
        ROS_INFO("Fiducial Slam will save the generated map");
    }
	
	// Set verbosity level if present
    nh.param("verbose_info", verboseInfo, false);


    if (doPoseEstimation) {
        double fiducialLen, errorThreshold;
        nh.param<double>("fiducial_len", fiducialLen, 0.14);
        nh.param<double>("pose_error_theshold", errorThreshold, 1.0);

        ftPub = ros::Publisher(
            nh.advertise<fiducial_msgs::FiducialTransformArray>("/fiducial_transforms", 1));
    } else {
        ft_sub = nh.subscribe("/fiducial_transforms", 1, &FiducialSlam::transformCallback, this);
    }

    ROS_INFO("Fiducial Slam ready");
}

auto node = unique_ptr<FiducialSlam>(nullptr);

void mySigintHandler(int sig) {
    if (node != nullptr) {
        if (node->use_read_only_map) {
            ROS_INFO("Fiducial Slam not saving map per read_only_map option");
        } else {
            node->fiducialMap.saveMap();
        }
    }

    ros::shutdown();
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "fiducial_slam", ros::init_options::NoSigintHandler);
    ros::NodeHandle nh("~");

    node = make_unique<FiducialSlam>(nh);
    signal(SIGINT, mySigintHandler);

    ros::Rate r(20);
    while (ros::ok()) {
        ros::spinOnce();
        r.sleep();
        node->fiducialMap.update();
    }

    return 0;
}
