#pragma once

#include <openr/iosxrsl/ServiceLayerAsyncInit.h>

#include <iosxrsl/sl_route_common.pb.h>
#include <iosxrsl/sl_route_ipv4.grpc.pb.h>
#include <iosxrsl/sl_route_ipv6.grpc.pb.h>
#include <iosxrsl/sl_route_ipv4.pb.h>
#include <iosxrsl/sl_route_ipv6.pb.h>

namespace openr {

class RShuttle;
extern RShuttle *route_shuttle;

class RShuttle {
public:
    explicit RShuttle(std::shared_ptr<grpc::Channel> Channel);

    std::shared_ptr<grpc::Channel> channel;
    service_layer::SLObjectOp route_op;
    service_layer::SLRoutev4Msg routev4_msg;
    service_layer::SLRoutev4MsgRsp routev4_msg_resp;
    service_layer::SLRoutev6Msg routev6_msg;
    service_layer::SLRoutev6MsgRsp routev6_msg_resp;

    // IPv4 and IPv6 string manipulation methods

    uint32_t IPv4ToLong(const char* address);

    std::string IPv6ToByteArrayString(const char* address);

    // IPv4 methods

    service_layer::SLRoutev4*
    routev4Add(std::string vrfName);

    void routev4Set(service_layer::SLRoutev4* routev4Ptr,
                    uint32_t prefix,
                    uint32_t prefixLen,
                    uint32_t adminDistance); 


    void routev4PathAdd(service_layer::SLRoutev4* routev4Ptr,
                        uint32_t nextHopAddress,
                        std::string nextHopIf);
 

    void routev4Op(service_layer::SLObjectOp routeOp,
                   unsigned int timeout=10);



    // IPv6 methods
    service_layer::SLRoutev6*
    routev6Add(std::string vrfName);
 

    void routev6Set(service_layer::SLRoutev6* routev6Ptr,
                    std::string prefix,
                    uint32_t prefixLen,
                    uint32_t adminDistance);

    void routev6PathAdd(service_layer::SLRoutev6* routev6Ptr,
                        std::string nextHopAddress,
                        std::string nextHopIf);
 

    void routev6Op(service_layer::SLObjectOp routeOp,
                   unsigned int timeout=10);

};


class SLVrf {
public:
    explicit SLVrf(std::shared_ptr<grpc::Channel> Channel);

    std::shared_ptr<grpc::Channel> channel;
    service_layer::SLRegOp vrf_op;
    service_layer::SLVrfRegMsg vrf_msg;
    service_layer::SLVrfRegMsgRsp vrf_msg_resp;

    void vrfRegMsgAdd(std::string vrfName);

    void vrfRegMsgAdd(std::string vrfName,
                      unsigned int adminDistance,
                      unsigned int vrfPurgeIntervalSeconds);

    void registerVrf(unsigned int addrFamily);

    void unregisterVrf(unsigned int addrFamily);

    void vrfOpv4();

    void vrfOpv6();

};

}
