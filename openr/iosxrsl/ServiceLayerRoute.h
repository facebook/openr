#pragma once

#include "ServiceLayerAsyncInit.h"
#include <iosxrsl/sl_route_common.pb.h>
#include <iosxrsl/sl_route_ipv4.grpc.pb.h>
#include <iosxrsl/sl_route_ipv6.grpc.pb.h>
#include <iosxrsl/sl_route_ipv4.pb.h>
#include <iosxrsl/sl_route_ipv6.pb.h>


namespace openr {

class IosxrslRoute {
public:
    explicit IosxrslRoute(std::shared_ptr<grpc::Channel> Channel);

    enum PathUpdateAction
    {
        IOSXRSL_RSHUTTLE_PATH_ADD,
        IOSXRSL_RSHUTTLE_PATH_DELETE,
    };

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

    void setVrfV4(std::string vrfName);


    service_layer::SLRoutev4*
    routev4Add();

    service_layer::SLRoutev4*
    routev4Add(std::string vrfName);


    void routev4Set(service_layer::SLRoutev4* routev4Ptr,
                    uint32_t prefix,
                    uint8_t prefixLen);

    void routev4Set(service_layer::SLRoutev4* routev4Ptr,
                    uint32_t prefix,
                    uint8_t prefixLen,
                    uint32_t adminDistance); 


    void routev4PathAdd(service_layer::SLRoutev4* routev4Ptr,
                        uint32_t nextHopAddress,
                        std::string nextHopIf);
 

    bool routev4Op(service_layer::SLObjectOp routeOp,
                   unsigned int timeout=10);


    bool insertAddBatchV4(std::string prefix,
                          uint8_t prefixLen,
                          uint32_t adminDistance,
                          std::string nextHopAddress,
                          std::string nextHopIf);

    bool insertDeleteBatchV4(std::string prefix,
                             uint8_t prefixLen);


    bool insertUpdateBatchV4(std::string prefix,
                             uint8_t prefixLen,
                             std::string nextHopAddress,
                             std::string nextHopIf,
                             PathUpdateAction action);

    bool insertUpdateBatchV4(std::string prefix,
                             uint8_t prefixLen,
                             uint32_t adminDistance,
                             std::string nextHopAddress,
                             std::string nextHopIf,
                             PathUpdateAction action);    

    void clearBatchV4();


    // Returns true if the prefix exists in Application RIB and route
    // gets populated with all the route attributes like Nexthop, adminDistance etc.

    bool getPrefixPathsV4(service_layer::SLRoutev4& route,
                          std::string vrfName,
                          std::string prefix,
                          uint8_t prefixLen,
                          unsigned int timeout=10);

    bool addPrefixPathV4(std::string prefix,
                         uint8_t prefixLen,
                         std::string nextHopAddress,
                         std::string nextHopIf);

    bool deletePrefixPathV4(std::string prefix,
                            uint8_t prefixLen,
                            std::string nextHopAddress,
                            std::string nextHopIf);

    // IPv6 methods

    void setVrfV6(std::string vrfName);


    service_layer::SLRoutev6*
    routev6Add();

    service_layer::SLRoutev6*
    routev6Add(std::string vrfName);
 

    void routev6Set(service_layer::SLRoutev6* routev6Ptr,
                    std::string prefix,
                    uint8_t prefixLen);


    void routev6Set(service_layer::SLRoutev6* routev6Ptr,
                    std::string prefix,
                    uint8_t prefixLen,
                    uint32_t adminDistance);

    void routev6PathAdd(service_layer::SLRoutev6* routev6Ptr,
                        std::string nextHopAddress,
                        std::string nextHopIf);
 

    bool routev6Op(service_layer::SLObjectOp routeOp,
                   unsigned int timeout=10);

    bool insertAddBatchV6(std::string prefix,
                          uint8_t prefixLen,
                          uint32_t adminDistance,
                          std::string nextHopAddress,
                          std::string nextHopIf);

    bool insertDeleteBatchV6(std::string prefix,
                             uint8_t prefixLen);

    bool insertUpdateBatchV6(std::string prefix,
                             uint8_t prefixLen,
                             std::string nextHopAddress,
                             std::string nextHopIf,
                             PathUpdateAction action);

    bool insertUpdateBatchV6(std::string prefix,
                             uint8_t prefixLen,
                             uint32_t adminDistance,
                             std::string nextHopAddress,
                             std::string nextHopIf,
                             PathUpdateAction action); 


    void clearBatchV6();

    // Returns true if the prefix exists in Application RIB and route
    // gets populated with all the route attributes like Nexthop, adminDistance etc.
    bool getPrefixPathsV6(service_layer::SLRoutev6& route,
                          std::string vrfName,
                          std::string prefix,
                          uint8_t prefixLen,
                          unsigned int timeout=10);

    bool addPrefixPathV6(std::string prefix,
                         uint8_t prefixLen,
                         std::string nextHopAddress,
                         std::string nextHopIf);

    bool deletePrefixPathV6(std::string prefix,
                            uint8_t prefixLen,
                            std::string nextHopAddress,
                            std::string nextHopIf);
};


class IosxrslVrf {
public:
    explicit IosxrslVrf(std::shared_ptr<grpc::Channel> Channel);

    std::shared_ptr<grpc::Channel> channel;
    service_layer::SLVrfRegMsg vrf_msg;
    service_layer::SLVrfRegMsgRsp vrf_msg_resp;

    void vrfRegMsgAdd(std::string vrfName);

    void vrfRegMsgAdd(std::string vrfName,
                      uint8_t adminDistance,
                      unsigned int vrfPurgeIntervalSeconds);

    bool registerVrf(unsigned int addrFamily);

    bool unregisterVrf(unsigned int addrFamily);

    bool vrfOpv4(service_layer::SLRegOp);

    bool vrfOpv6(service_layer::SLRegOp);

};

}
