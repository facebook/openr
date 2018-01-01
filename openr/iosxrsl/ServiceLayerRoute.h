#pragma once

#include <openr/iosxrsl/ServiceLayerAsyncInit.h>
#include <iosxrsl/sl_route_common.pb.h>
#include <iosxrsl/sl_route_ipv4.grpc.pb.h>
#include <iosxrsl/sl_route_ipv6.grpc.pb.h>
#include <iosxrsl/sl_route_ipv4.pb.h>
#include <iosxrsl/sl_route_ipv6.pb.h>

namespace openr {

extern std::shared_ptr<grpc::Channel> route_channel;

class RShuttle {
public:
    explicit RShuttle(std::shared_ptr<grpc::Channel> Channel);

    std::shared_ptr<grpc::Channel> channel;
    service_layer::SLObjectOp route_op;
    service_layer::SLRoutev4Msg routev4_msg;
    service_layer::SLRoutev4MsgRsp routev4_msg_resp;
    service_layer::SLRoutev6Msg routev6_msg;
    service_layer::SLRoutev6MsgRsp routev6_msg_resp;

    std::map<std::string, int> prefix_map_v4;
    std::map<std::string, int> prefix_map_v6;


    // IPv4 and IPv6 string manipulation methods

    std::string longToIpv4(uint32_t nlprefix);
    uint32_t ipv4ToLong(const char* address);
    std::string ipv6ToByteArrayString(const char* address);
    std::string ByteArrayStringtoIpv6(std::string ipv6ByteArray);

    // IPv4 methods

    service_layer::SLRoutev4*
    routev4Add(std::string vrfName);


    void routev4Set(service_layer::SLRoutev4* routev4Ptr,
                    uint32_t prefix,
                    uint32_t prefixLen);

    void routev4Set(service_layer::SLRoutev4* routev4Ptr,
                    uint32_t prefix,
                    uint32_t prefixLen,
                    uint32_t adminDistance); 


    void routev4PathAdd(service_layer::SLRoutev4* routev4Ptr,
                        uint32_t nextHopAddress,
                        std::string nextHopIf);
 

    bool routev4Op(service_layer::SLObjectOp routeOp,
                   unsigned int timeout=10);


    void insertAddBatchV4(std::string vrfName,
                          std::string prefix,
                          uint32_t prefixLen,
                          uint32_t adminDistance,
                          std::string nextHopAddress,
                          std::string nextHopIf);

    void insertDeleteBatchV4(std::string vrfName,
                             std::string prefix,
                             uint32_t prefixLen);

    
    void clearBatchV4();


    // Returns true if the prefix exists in Application RIB and route
    // gets populated with all the route attributes like Nexthop, adminDistance etc.

    bool getPrefixPathsV4(service_layer::SLRoutev4& route,
                          std::string vrfName,
                          std::string prefix,
                          uint32_t prefixLen,
                          unsigned int timeout=10);


    // IPv6 methods
    service_layer::SLRoutev6*
    routev6Add(std::string vrfName);
 

    void routev6Set(service_layer::SLRoutev6* routev6Ptr,
                    std::string prefix,
                    uint32_t prefixLen);


    void routev6Set(service_layer::SLRoutev6* routev6Ptr,
                    std::string prefix,
                    uint32_t prefixLen,
                    uint32_t adminDistance);

    void routev6PathAdd(service_layer::SLRoutev6* routev6Ptr,
                        std::string nextHopAddress,
                        std::string nextHopIf);
 

    bool routev6Op(service_layer::SLObjectOp routeOp,
                   unsigned int timeout=10);

    void insertAddBatchV6(std::string vrfName,
                          std::string prefix,
                          uint32_t prefixLen,
                          uint32_t adminDistance,
                          std::string nextHopAddress,
                          std::string nextHopIf);

    void insertDeleteBatchV6(std::string vrfName,
                             std::string prefix,
                             uint32_t prefixLen);


    void clearBatchV6();

    // Returns true if the prefix exists in Application RIB and route
    // gets populated with all the route attributes like Nexthop, adminDistance etc.
    bool getPrefixPathsV6(service_layer::SLRoutev6& route,
                          std::string vrfName,
                          std::string prefix,
                          uint32_t prefixLen,
                          unsigned int timeout=10);


};


class SLVrf {
public:
    explicit SLVrf(std::shared_ptr<grpc::Channel> Channel);

    std::shared_ptr<grpc::Channel> channel;
    service_layer::SLVrfRegMsg vrf_msg;
    service_layer::SLVrfRegMsgRsp vrf_msg_resp;

    void vrfRegMsgAdd(std::string vrfName);

    void vrfRegMsgAdd(std::string vrfName,
                      unsigned int adminDistance,
                      unsigned int vrfPurgeIntervalSeconds);

    bool registerVrf(unsigned int addrFamily);

    bool unregisterVrf(unsigned int addrFamily);

    bool vrfOpv4(service_layer::SLRegOp);

    bool vrfOpv6(service_layer::SLRegOp);

};

}
