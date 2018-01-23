#pragma once

#include <stdint.h>
#include <thread>
#include <condition_variable>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <glog/logging.h>
#include <arpa/inet.h>

#include <grpc++/grpc++.h>

#include <xrtelemetry/telemetry.pb.h>
#include <xrtelemetry/telemetry.grpc.pb.h>
#include <xrtelemetry/cisco_ios_xr_ipv6_nd_oper/ipv6_node_discovery/nodes/node/neighbor_interfaces/neighbor_interface/host_addresses/host_address/ipv6_nd_neighbor_entry.pb.h>

namespace openr {

std::string gpbMsgToJson(const google::protobuf::Message& message);

using SensorPaths = std::map<std::string, std::string>;

class TelemetryDecode;
using DecodeSensorPathGPB = void (TelemetryDecode::*)(const telemetry::TelemetryRowGPB& telemetry_gpb_row);
using DecodeSensorPathMapGPB = std::unordered_map<std::string, DecodeSensorPathGPB>;
using DecodeSensorPathGPBKV = void (TelemetryDecode::*)(const telemetry::TelemetryField& telemetry_gpbkv_field);
using DecodeSensorPathMapGPBKV = std::unordered_map<std::string, DecodeSensorPathGPBKV>;

class TelemetryDecode {
public:
    explicit TelemetryDecode();
    ~TelemetryDecode();

    DecodeSensorPathMapGPB decodeSensorPathMapGPB;
    DecodeSensorPathMapGPBKV decodeSensorPathMapGPBKV;

    // decode Telemetry data and call hooks based on encoding path
    void DecodeTelemetryData(const telemetry::Telemetry& telemetry_data);

    // decode Telemetry data and call hooks based on encoding path
    void DecodeTelemetryDataGPB(const telemetry::Telemetry& telemetry_data);

    void DecodeTelemetryDataGPBKV(const telemetry::Telemetry& telemetry_data);

    // Helper Methods called based on different Sensor Paths
    void DecodeIPv6NeighborsGPB(const ::telemetry::TelemetryRowGPB& telemetry_gpb_row);

    void DecodeIPv6NeighborsGPBKV(const ::telemetry::TelemetryField& telemetry_gpbkv_field);

};

}
