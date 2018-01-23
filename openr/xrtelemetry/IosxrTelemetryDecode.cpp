#include "IosxrTelemetryDecode.h" 
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/json_util.h>


namespace openr {

namespace {
    SensorPaths sensorPaths = {
                                {
                                  "iosxr-ipv6-nd-address",
                                  "Cisco-IOS-XR-ipv6-nd-oper:ipv6-node-discovery/nodes/node/neighbor-interfaces/neighbor-interface/host-addresses/host-address"
                                }
                              };
}

std::string
gpbMsgToJson(const google::protobuf::Message& message)
{
    std::string json_string;

    google::protobuf::util::JsonPrintOptions options;

    options.add_whitespace = true;
    options.always_print_primitive_fields = true;
    options.preserve_proto_field_names = true;
    
    auto status = google::protobuf::util::
               MessageToJsonString(message, 
                                   &json_string, 
                                   options);

    if (status.ok()) { 
        return json_string;
    } else {
        LOG(ERROR) << "Failed to convert protobuf message to json";
        LOG(ERROR) << "Error: " << status.error_message();
        LOG(ERROR) << "Error Code: " << status.error_code();
        return "";
    }
}

TelemetryDecode::TelemetryDecode()
{
   decodeSensorPathMapGPB.insert(
                 std::make_pair(
                      sensorPaths["iosxr-ipv6-nd-address"],
                      &TelemetryDecode::DecodeIPv6NeighborsGPB));

   decodeSensorPathMapGPBKV.insert(
                 std::make_pair(
                      sensorPaths["iosxr-ipv6-nd-address"],
                      &TelemetryDecode::DecodeIPv6NeighborsGPBKV));
}

TelemetryDecode::~TelemetryDecode() {};


void
TelemetryDecode::
DecodeIPv6NeighborsGPB(const ::telemetry::TelemetryRowGPB& telemetry_gpb_row)
{

    using namespace cisco_ios_xr_ipv6_nd_oper::
                    ipv6_node_discovery::
                    nodes::node::neighbor_interfaces::
                    neighbor_interface::host_addresses::host_address;

    std::cout << "\n\n\n\n############################\n\n";
    auto ipv6_nd_neigh_entry_keys = ipv6_nd_neighbor_entry_KEYS();
    ipv6_nd_neigh_entry_keys.ParseFromString(telemetry_gpb_row.keys());

    VLOG(3) << "IPv6 ND entry keys \n"
            << gpbMsgToJson(ipv6_nd_neigh_entry_keys);


    auto ipv6_nd_neigh_entry = ipv6_nd_neighbor_entry();
    ipv6_nd_neigh_entry.ParseFromString(telemetry_gpb_row.content());

    VLOG(3) << "IPv6 ND entry \n"
            << gpbMsgToJson(ipv6_nd_neigh_entry);

    std::cout << "\n\n############################\n\n\n";
}

void
TelemetryDecode::
DecodeIPv6NeighborsGPBKV(const ::telemetry::TelemetryField& telemetry_gpbkv_field)
{

    VLOG(2) << "OLA2!!!!!";

}

void
TelemetryDecode::DecodeTelemetryDataGPB(const telemetry::Telemetry& telemetry_data)
{

    auto telemetry_gpb_table = telemetry_data.data_gpb();    
    for (auto row_index=0;
         row_index < telemetry_gpb_table.row_size();)
    {
        auto telemetry_gpb_row = telemetry_gpb_table.row(row_index);
        VLOG(3) << "Telemetry GPB row \n"
                << gpbMsgToJson(telemetry_gpb_row);
        (this->*decodeSensorPathMapGPB[telemetry_data.encoding_path()])(telemetry_gpb_row);
        row_index++;
    }
}

void
TelemetryDecode::DecodeTelemetryDataGPBKV(const telemetry::Telemetry& telemetry_data)
{
    
}  


void
TelemetryDecode::DecodeTelemetryData(const telemetry::Telemetry& telemetry_data)
{
    VLOG(2) << "Telemetry Data: \n"
            << gpbMsgToJson(telemetry_data);

    VLOG(2) << "Encoding Path : \n"
            << telemetry_data.encoding_path();

    if (telemetry_data.has_data_gpb()) {
        DecodeTelemetryDataGPB(telemetry_data);

    } else {
        DecodeTelemetryDataGPBKV(telemetry_data);
    }
 
}

}
