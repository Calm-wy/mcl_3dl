#include <chrono>

#include <ros/ros.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/PointCloud2.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/PoseArray.h>

#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_datatypes.h>

#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_types.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl/point_types.h>
#include <pcl/conversions.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/registration/icp.h>

#include <pf.hpp>
#include <vec3.hpp>
#include <quat.hpp>

	
class mcl_3dl_node
{
private:
	ros::Subscriber sub_cloud;
	ros::Subscriber sub_mapcloud;
	ros::Subscriber sub_odom;
	ros::Publisher pub_particle;
	ros::Publisher pub_debug;
	ros::Publisher pub_mapcloud;

	tf::TransformListener tfl;

	class state: public pf::particleBase<float>
	{
	public:
		vec3 pos;
		quat rot;
		struct
		{
			vec3 lin;
			vec3 ang;
		} vel;
		virtual float &operator[](size_t i) override
		{
			switch(i)
			{
			case 0: return pos.x;
			case 1: return pos.y;
			case 2: return pos.z;
			case 3: return rot.x;
			case 4: return rot.y;
			case 5: return rot.z;
			case 6: return rot.w;
			case 7: return vel.lin.x;
			case 8: return vel.lin.y;
			case 9: return vel.lin.z;
			case 10: return vel.ang.x;
			case 11: return vel.ang.y;
			case 12: return vel.ang.z;
			}
			return pos.x;
		};
		virtual const size_t size() override
		{
			return 13;
		};
		state()
		{
		};
		state(const vec3 pos, const quat rot, const vec3 vel_lin, const vec3 vel_ang)
		{
			this->pos = pos;
			this->rot = rot;
			this->vel.lin = vel_lin;
			this->vel.ang = vel_ang;
		};
		void transform(pcl::PointCloud<pcl::PointXYZ> &pc) const
		{
			auto r = rot.normalized();
			for(auto &p: pc.points)
			{
				auto t = r * vec3(p.x, p.y, p.z) + pos;
				p.x = t.x;
				p.y = t.y;
				p.z = t.z;
			}
		}
	};
	std::shared_ptr<pf::particle_filter<state>> pf;

	pcl::PointCloud<pcl::PointXYZ>::Ptr pc_map;
	pcl::KdTreeFLANN<pcl::PointXYZ>::Ptr kdtree;
	void cb_mapcloud(const sensor_msgs::PointCloud2::ConstPtr &msg)
	{
		ROS_INFO("map received");
		pcl::PointCloud<pcl::PointXYZ> pc_tmp;
		pcl::fromROSMsg(*msg, pc_tmp);

		pc_map.reset(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::VoxelGrid<pcl::PointXYZ> ds;
		ds.setInputCloud(pc_tmp.makeShared());
		ds.setLeafSize(0.1, 0.1, 0.1);
		ds.filter(*pc_map);
		kdtree.reset(new  pcl::KdTreeFLANN<pcl::PointXYZ>);
		kdtree->setInputCloud(pc_map);
		pc_local_accum.reset(new pcl::PointCloud<pcl::PointXYZ>);
		frame_num = 0;
		has_map = true;
	}

	bool update_map;

	ros::Time odom_last;
	bool has_map;
	void cb_odom(const nav_msgs::Odometry::ConstPtr &msg)
	{
		if(odom_last != ros::Time(0))
		{
			float dt = (msg->header.stamp - odom_last).toSec();
			//ROS_INFO("odom %0.3f", dt);
			pf->predict([&](state &s)
					{
						s.vel.lin.x = msg->twist.twist.linear.x;
						s.vel.lin.y = msg->twist.twist.linear.y;
						s.vel.lin.z = msg->twist.twist.linear.z;
						s.vel.ang.x = msg->twist.twist.angular.x;
						s.vel.ang.y = msg->twist.twist.angular.y;
						s.vel.ang.z = msg->twist.twist.angular.z;

						s.rot.normalize();

						s.pos += (s.rot * s.vel.lin) * dt;
						s.rot = s.rot * (quat(vec3(1.0, 0.0, 0.0), s.vel.ang.x * dt)
								* quat(vec3(0.0, 1.0, 0.0), s.vel.ang.y * dt)
								* quat(vec3(0.0, 0.0, 1.0), s.vel.ang.z * dt));
					});
		}
		odom_last = msg->header.stamp;
	}
	std::random_device seed_gen;
	std::default_random_engine engine;
	std::map<std::string, bool> frames;
	std::vector<std::string> frames_v;
	size_t frame_num;
	pcl::PointCloud<pcl::PointXYZ>::Ptr pc_local_accum;
	void cb_cloud(const sensor_msgs::PointCloud2::ConstPtr &msg)
	{
		if(!has_map) return;
		if(frames.find(msg->header.frame_id) == frames.end())
		{
			frames[msg->header.frame_id] = true;
			frames_v.push_back(msg->header.frame_id);
		}

		if(frames_v[frame_num].compare(msg->header.frame_id) != 0) return;
		frame_num ++;
		if(frame_num >= frames_v.size()) frame_num = 0;

		//ROS_INFO("cloud %s  %d/%d", msg->header.frame_id.c_str(), frame_num, frames_v.size());

		sensor_msgs::PointCloud2 pc_bl;
		if(!pcl_ros::transformPointCloud("base_link", *msg, pc_bl, tfl))
		{
			return;
		}
		pcl::PointCloud<pcl::PointXYZ>::Ptr pc_tmp(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::fromROSMsg(pc_bl, *pc_tmp);
		*pc_local_accum += *pc_tmp;

		if(frame_num != 0) return;

		const auto ts = std::chrono::high_resolution_clock::now();

		pcl::PointCloud<pcl::PointXYZ>::Ptr pc_local(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::VoxelGrid<pcl::PointXYZ> ds;
		ds.setInputCloud(pc_local_accum);
		ds.setLeafSize(0.5, 0.5, 0.1);
		ds.filter(*pc_local);
		pc_local_accum.reset(new pcl::PointCloud<pcl::PointXYZ>);

		std::uniform_real_distribution<float> ud(0.0, 1.0);
		pc_local->points.erase(
				std::remove_if(pc_local->points.begin(), pc_local->points.end(),
					[&](const pcl::PointXYZ &p)
					{
						if(p.x*p.x + p.y*p.y > 8.0*8.0) return true;
						if(p.x*p.x + p.y*p.y < 0.8*0.8) return true;
						return ud(engine) > 0.1;
					}), pc_local->points.end());
		pc_local->width = 1;
		pc_local->height = pc_local->points.size();
		
		pcl::PointCloud<pcl::PointXYZ>::Ptr pc_particle(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::PointCloud<pcl::PointXYZ>::Ptr pc_dummy(new pcl::PointCloud<pcl::PointXYZ>);

		std::vector<int> id(1);
		std::vector<float> sqdist(1);
		pf->measure([&](const state &s)->float
				{
					float score = 0;
					*pc_particle = *pc_local;
					s.transform(*pc_particle);

					for(auto &p: pc_particle->points)
					{
						if(kdtree->nearestKSearch(p, 1, id, sqdist))
						{
							if(sqdist[0] > 0.5*0.5) sqdist[0] = 0.5*0.5;
							else if(sqdist[0] < 0.05*0.05) sqdist[0] = 0.05*0.05;
							score += sqdist[0];
						}
					}
					score /= pc_particle->points.size();
					return 1.0 / sqrtf(score);
				});
		
		auto e = pf->max();
		*pc_particle = *pc_local;
		e.transform(*pc_particle);
		sensor_msgs::PointCloud pc;
		pc.header.stamp = ros::Time::now();
		pc.header.frame_id = "map";
		for(auto &p: pc_particle->points)
		{
			geometry_msgs::Point32 pp;
			pp.x = p.x;
			pp.y = p.y;
			pp.z = p.z;
			pc.points.push_back(pp);
			
			if(update_map)
			{
				if(kdtree->nearestKSearch(p, 1, id, sqdist))
				{
					if(sqdist[0] > 1.0*1.0)
					{
						pc_map->insert(pc_map->end(), p);
					}
				}
			}
		}
		pub_debug.publish(pc);

		pf->resample();
		pf->noise(
				state(
					vec3(0.01, 0.01, 0.001),
					quat(0.001, 0.001, 0.01, 0.01),
					vec3(0.001, 0.001, 0.001),
					vec3(0.001, 0.001, 0.001)
					)
				);	
		
		const auto tnow = std::chrono::high_resolution_clock::now();
		ROS_INFO("MCL (%0.3f sec.)",
				std::chrono::duration<float>(tnow - ts).count());
	}

public:
	mcl_3dl_node(int argc, char *argv[]):
		engine(seed_gen())
	{
		ros::NodeHandle nh("~");

		sub_cloud = nh.subscribe("cloud", 20, &mcl_3dl_node::cb_cloud, this);
		sub_odom = nh.subscribe("odom", 1000, &mcl_3dl_node::cb_odom, this);
		sub_mapcloud = nh.subscribe("mapcloud", 1, &mcl_3dl_node::cb_mapcloud, this);
		pub_particle = nh.advertise<geometry_msgs::PoseArray>("particles", 1, true);
		pub_debug = nh.advertise<sensor_msgs::PointCloud>("debug", 5, true);
		pub_mapcloud = nh.advertise<sensor_msgs::PointCloud2>("updated_map", 1, true);

		nh.param("update_map", update_map, false);

		pf.reset(new pf::particle_filter<state>(400));
		
		pf->init(
				state(
					vec3(-0.5, 1.0, 3.2),
					quat(0.0, 0.0, 0.0, 1.0),
					vec3(0.0, 0.0, 0.0),
					vec3(0.0, 0.0, 0.0)
					), 
				state(
					vec3(0.5, 0.5, 0.1),
					quat(0.0, 0.0, 0.2, 0.2),
					vec3(0.0, 0.0, 0.0),
					vec3(0.0, 0.0, 0.0)
					));
	}
	~mcl_3dl_node()
	{
	}
	void spin()
	{
		ros::Rate rate(5);
		int cnt = 0;
		while(ros::ok())
		{
			rate.sleep();
			ros::spinOnce();

			geometry_msgs::PoseArray pa;
			pa.header.stamp = ros::Time::now();
			pa.header.frame_id = "map";
			for(size_t i = 0; i < pf->get_particle_size(); i ++)
			{
				geometry_msgs::Pose pm;
				auto p = pf->get_particle(i);
				p.rot.normalize();
				pm.position.x = p.pos.x;
				pm.position.y = p.pos.y;
				pm.position.z = p.pos.z;
				pm.orientation.x = p.rot.x;
				pm.orientation.y = p.rot.y;
				pm.orientation.z = p.rot.z;
				pm.orientation.w = p.rot.w;
				pa.poses.push_back(pm);
			}
			pub_particle.publish(pa);

			cnt ++;
			if(update_map)
			{
				if(cnt % 10 == 0)
				{
					sensor_msgs::PointCloud2 out;
					pcl::toROSMsg(*pc_map, out);
					pub_mapcloud.publish(out);

					kdtree.reset(new  pcl::KdTreeFLANN<pcl::PointXYZ>);
					kdtree->setInputCloud(pc_map);
				}
			}
		}
	}
};

int main(int argc, char *argv[])
{
	ros::init(argc, argv, "mcl_3dl");

	mcl_3dl_node mcl(argc, argv);
	mcl.spin();

	return 0;
}


