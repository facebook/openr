#include <openr/iosxrsl/ServiceLayerRoute.h>
#include <arpa/inet.h>
#include <google/protobuf/text_format.h>
#include <csignal>

using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::CompletionQueue;
using grpc::Status;
using service_layer::SLInitMsg;
using service_layer::SLVersion;
using service_layer::SLGlobal;


namespace openr {

RShuttle* route_shuttle;

RShuttle::RShuttle(std::shared_ptr<grpc::Channel> Channel)
    : channel(Channel) {} 


uint32_t RShuttle::IPv4ToLong(const char* address)
{   
    struct sockaddr_in sa; 
    if (inet_pton(AF_INET, address, &(sa.sin_addr)) != 1) {
        std::cerr << "Invalid IPv4 address " << address << std::endl;
        return 0;
    }
    
    return ntohl(sa.sin_addr.s_addr);
}

std::string RShuttle::IPv6ToByteArrayString(const char* address)
{   
    //const char *ipv6str = address;
    struct in6_addr ipv6data;
    if (inet_pton(AF_INET6, address, &ipv6data) != 1 ) {
        std::cerr << "Invalid IPv6 address " << address << std::endl;
        return 0;
    }

    const char *ptr(reinterpret_cast<const char*>(&ipv6data.s6_addr));
    std::string ipv6_charstr(ptr, ptr+16);
    return ipv6_charstr;
}


service_layer::SLRoutev4* 
    RShuttle::routev4Add(std::string vrfName)
{
    routev4_msg.set_vrfname(vrfName);
    return routev4_msg.add_routes();
}


void RShuttle::routev4Set(service_layer::SLRoutev4* routev4Ptr,
                          uint32_t prefix,
                          uint32_t prefixLen,
                          uint32_t adminDistance)
{
    routev4Ptr->set_prefix(prefix);
    routev4Ptr->set_prefixlen(prefixLen);
    routev4Ptr->mutable_routecommon()->set_admindistance(adminDistance);
}

void RShuttle::routev4PathAdd(service_layer::SLRoutev4* routev4Ptr,
                              uint32_t nextHopAddress,
                              std::string nextHopIf)
{
    
    auto routev4PathPtr = routev4Ptr->add_pathlist();
    routev4PathPtr->mutable_nexthopaddress()->set_v4address(nextHopAddress);
    routev4PathPtr->mutable_nexthopinterface()->set_name(nextHopIf);
}

void RShuttle::routev4Op(service_layer::SLObjectOp routeOp,
                         unsigned int timeout)
{

    // Convert ADD to UPDATE automatically, it will solve both the 
    // conditions - add or update.

    if (routeOp == service_layer::SL_OBJOP_ADD) {
        routeOp = service_layer::SL_OBJOP_UPDATE;
    }

    route_op = routeOp;
    routev4_msg.set_oper(route_op);

    auto stub_ = service_layer::SLRoutev4Oper::NewStub(channel); 

    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    grpc::ClientContext context;

    // Storage for the status of the RPC upon completion.
    grpc::Status status;

    // Set timeout for RPC
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(timeout);

    context.set_deadline(deadline);

    //Issue the RPC         
    std::string s;

    if (google::protobuf::TextFormat::PrintToString(routev4_msg, &s)) {
        std::cout << "\n\n###########################\n" ;
        std::cout << "Transmitted message: IOSXR-SL RouteV4 " << s;
        std::cout << "###########################\n\n\n" ;
    } else {
        std::cerr << "\n\n###########################\n" ;
        std::cerr << "Message not valid (partial content: "
                  << routev4_msg.ShortDebugString() << ")\n";
        std::cerr << "###########################\n\n\n" ;
    }

    status = stub_->SLRoutev4Op(&context, routev4_msg, &routev4_msg_resp);

    if (status.ok()) {
        std::cout << "RPC call was successful, checking response..." << std::endl;


        if (routev4_msg_resp.statussummary().status() ==
               service_layer::SLErrorStatus_SLErrno_SL_SUCCESS) {

            std::cout << "IPv4 Route Operation:"<< route_op << " Successful" << std::endl;
        } else {
            std::cerr << "Error code for IPv4 Route Operation:" << route_op << " is 0x" << std::hex << routev4_msg_resp.statussummary().status() << std::endl;

            // Print Partial failures within the batch if applicable
            if (routev4_msg_resp.statussummary().status() ==
                    service_layer::SLErrorStatus_SLErrno_SL_SOME_ERR) {
                for (int result = 0; result < routev4_msg_resp.results_size(); result++) {
                      auto slerr_status = static_cast<int>(routev4_msg_resp.results(result).errstatus().status());
                      std::cerr << "Error code for prefix: " << routev4_msg_resp.results(result).prefix() << " prefixlen: " << routev4_msg_resp.results(result).prefixlen()<<" is 0x"<< std::hex << slerr_status << std::endl;
                }
            }
        }
    } else {
        std::cerr << "RPC failed, error code is " << status.error_code() << std::endl;
    }
}



service_layer::SLRoutev6*
    RShuttle::routev6Add(std::string vrfName)
{
    routev6_msg.set_vrfname(vrfName);
    return routev6_msg.add_routes();
}


void RShuttle::routev6Set(service_layer::SLRoutev6* routev6Ptr,
                          std::string prefix,
                          uint32_t prefixLen,
                          uint32_t adminDistance)
{
    routev6Ptr->set_prefix(prefix);
    routev6Ptr->set_prefixlen(prefixLen);
    routev6Ptr->mutable_routecommon()->set_admindistance(adminDistance);
}

void RShuttle::routev6PathAdd(service_layer::SLRoutev6* routev6Ptr,
                              std::string nextHopAddress,
                              std::string nextHopIf)
{

    auto routev6PathPtr = routev6Ptr->add_pathlist();
    routev6PathPtr->mutable_nexthopaddress()->set_v6address(nextHopAddress);
    routev6PathPtr->mutable_nexthopinterface()->set_name(nextHopIf);
}

void RShuttle::routev6Op(service_layer::SLObjectOp routeOp,
                         unsigned int timeout)
{                      

    // Convert ADD to UPDATE automatically, it will solve both the 
    // conditions - add or update.
    
    if (routeOp == service_layer::SL_OBJOP_ADD) {
        routeOp = service_layer::SL_OBJOP_UPDATE;
    }
    
    route_op = routeOp;
    routev6_msg.set_oper(route_op);


    auto stub_ = service_layer::SLRoutev6Oper::NewStub(channel);

    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    grpc::ClientContext context;

    // Storage for the status of the RPC upon completion.
    grpc::Status status;

    // Set timeout for RPC
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(timeout);

    context.set_deadline(deadline);


    //Issue the RPC         
    std::string s;

    if (google::protobuf::TextFormat::PrintToString(routev6_msg, &s)) {
        std::cout << "\n\n###########################\n" ;
        std::cout << "Transmitted message: IOSXR-SL RouteV6 " << s;
        std::cout << "###########################\n\n\n" ;
    } else {
        std::cerr << "\n\n###########################\n" ;
        std::cerr << "Message not valid (partial content: "
                  << routev6_msg.ShortDebugString() << ")\n";
        std::cerr << "###########################\n\n\n" ;
    }

    //Issue the RPC         

    status = stub_->SLRoutev6Op(&context, routev6_msg, &routev6_msg_resp);

    if (status.ok()) {
        std::cout << "RPC call was successful, checking response..." << std::endl;


        if (routev6_msg_resp.statussummary().status() ==
               service_layer::SLErrorStatus_SLErrno_SL_SUCCESS) {

            std::cout << "IPv6 Route Operation:"<< route_op << " Successful" << std::endl;
        } else {
            std::cerr << "Error code for IPv6 Route Operation:" << route_op << " is 0x" << std::hex << routev6_msg_resp.statussummary().status() << std::endl;

            // Print Partial failures within the batch if applicable
            if (routev6_msg_resp.statussummary().status() ==
                    service_layer::SLErrorStatus_SLErrno_SL_SOME_ERR) {
                for (int result = 0; result < routev6_msg_resp.results_size(); result++) {
                      auto slerr_status = static_cast<int>(routev6_msg_resp.results(result).errstatus().status());
                      std::cerr << "Error code for prefix: " << routev6_msg_resp.results(result).prefix() << " prefixlen: " << routev6_msg_resp.results(result).prefixlen()<<" is 0x"<< std::hex << slerr_status << std::endl;

                }
            }
        }
    } else {
        std::cerr << "RPC failed, error code is " << status.error_code() << std::endl;
    }
}




SLVrf::SLVrf(std::shared_ptr<grpc::Channel> Channel)
    : channel(Channel) {}

// Overloaded variant of vrfRegMsgAdd without adminDistance and Purgeinterval
// Suitable for VRF UNREGISTER and REGISTER operations

void SLVrf::vrfRegMsgAdd(std::string vrfName) {

    // Get a pointer to a new vrf_reg entry in vrf_msg
    service_layer::SLVrfReg* vrf_reg = vrf_msg.add_vrfregmsgs();

    // Populate the new vrf_reg entry
    vrf_reg->set_vrfname(vrfName);
}

// Overloaded variant of vrfRegMsgAdd with adminDistance and Purgeinterval
// Suitable for VRF REGISTER

void SLVrf::vrfRegMsgAdd(std::string vrfName,
                         unsigned int adminDistance,
                         unsigned int vrfPurgeIntervalSeconds) {

    // Get a pointer to a new vrf_reg entry in vrf_msg
    service_layer::SLVrfReg* vrf_reg = vrf_msg.add_vrfregmsgs();

    // Populate the new vrf_reg entry
    vrf_reg->set_vrfname(vrfName);
    vrf_reg->set_admindistance(adminDistance);
    vrf_reg->set_vrfpurgeintervalseconds(vrfPurgeIntervalSeconds);
}


void SLVrf::registerVrf(unsigned int addrFamily) {

    // Send an RPC for VRF registrations

    switch(addrFamily) {
    case AF_INET:
        vrf_op = service_layer::SL_REGOP_REGISTER;
        vrfOpv4();

        // RPC EOF to cleanup any previous stale routes
        vrf_op = service_layer::SL_REGOP_EOF;
        vrfOpv4();

        break;

    case AF_INET6:
        vrf_op = service_layer::SL_REGOP_REGISTER;
        vrfOpv6();

        // RPC EOF to cleanup any previous stale routes
        vrf_op = service_layer::SL_REGOP_EOF;
        vrfOpv6();

        break;            

    default:
        std::cout << "Invalid Address family, skipping.." << std::endl;
        break;
    }

}

void SLVrf::unregisterVrf(unsigned int addrFamily) {

    //  When done with the VRFs, RPC Delete Registration

    switch(addrFamily) {
    case AF_INET:
        std::cout << "IPv4 VRF Operation" << std::endl;

        vrf_op = service_layer::SL_REGOP_UNREGISTER;
        vrfOpv4();
            
        break;

    case AF_INET6:
        std::cout << "IPv6 VRF Operation" << std::endl;
        
        vrf_op = service_layer::SL_REGOP_UNREGISTER;
        vrfOpv6();
        
        break;

    default:
        std::cout << "Invalid Address family, skipping.." << std::endl;
        break;
    }
}

void SLVrf::vrfOpv4() {
    // Set up the RouteV4Oper Stub
    auto stub_ = service_layer::SLRoutev4Oper::NewStub(channel);

    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    grpc::ClientContext context;

    // Storage for the status of the RPC upon completion.
    grpc::Status status;        

    unsigned int timeout = 10;
        // Set timeout for API
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(timeout);

    context.set_deadline(deadline);

    // Set up vrfRegMsg Operation

    vrf_msg.set_oper(vrf_op);

    std::string s;

    if (google::protobuf::TextFormat::PrintToString(vrf_msg, &s)) {
        std::cout << "\n\n###########################\n" ;
        std::cout << "Transmitted message: IOSXR-SL VRF " << s;
        std::cout << "###########################\n\n\n" ;
    } else {
        std::cerr << "\n\n###########################\n" ;
        std::cerr << "Message not valid (partial content: "
                  << vrf_msg.ShortDebugString() << ")\n";
        std::cerr << "###########################\n\n\n" ;
    }


    //Issue the RPC         

    status = stub_->SLRoutev4VrfRegOp(&context, vrf_msg, &vrf_msg_resp);

    if (status.ok()) {
        std::cout << "RPC call was successful, checking response..." << std::endl;


        if (vrf_msg_resp.statussummary().status() ==
               service_layer::SLErrorStatus_SLErrno_SL_SUCCESS) {

            std::cout << "IPv4 Vrf Operation:"<< vrf_op << " Successful" << std::endl;
        } else {
            std::cerr << "Error code for VRF Operation:" << vrf_op << " is 0x" << std::hex << vrf_msg_resp.statussummary().status() << std::endl;

            // Print Partial failures within the batch if applicable
            if (vrf_msg_resp.statussummary().status() ==
                    service_layer::SLErrorStatus_SLErrno_SL_SOME_ERR) {
                for (int result = 0; result < vrf_msg_resp.results_size(); result++) {
                      auto slerr_status = static_cast<int>(vrf_msg_resp.results(result).errstatus().status());
                      std::cerr << "Error code for vrf " << vrf_msg_resp.results(result).vrfname() << " is 0x" << std::hex << slerr_status << std::endl;
                }
            } 
        }
    } else {
        std::cerr << "RPC failed, error code is " << status.error_code() << std::endl;
    }
}
                    
void SLVrf::vrfOpv6() {

    // Set up the RouteV4Oper Stub
    auto stub_ = service_layer::SLRoutev6Oper::NewStub(channel);


    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    grpc::ClientContext context;
    
    // Storage for the status of the RPC upon completion.
    grpc::Status status;
    
    unsigned int timeout = 10;
    // Set timeout for API
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(timeout);
     
    context.set_deadline(deadline);

    // Set up vrfRegMsg Operation

    vrf_msg.set_oper(vrf_op);

    std::string s;

    if (google::protobuf::TextFormat::PrintToString(vrf_msg, &s)) {
        std::cout << "\n\n###########################\n" ;
        std::cout << "Transmitted message: IOSXR-SL VRF " << s;
        std::cout << "###########################\n\n\n" ;
    } else {
        std::cerr << "\n\n###########################\n" ;
        std::cerr << "Message not valid (partial content: "
                  << vrf_msg.ShortDebugString() << ")\n";
        std::cerr << "###########################\n\n\n" ;
    }


    //Issue the RPC         

    status = stub_->SLRoutev6VrfRegOp(&context, vrf_msg, &vrf_msg_resp);

    if (status.ok()) {
        std::cout << "RPC call was successful, checking response..." << std::endl;
        if (vrf_msg_resp.statussummary().status() ==
               service_layer::SLErrorStatus_SLErrno_SL_SUCCESS) {
            std::cout << "IPv6 Vrf Operation: "<< vrf_op << " successful" << std::endl;
        } else {
            std::cerr << "Error code for VRF Operation:" << vrf_op << " is 0x" << std::hex << vrf_msg_resp.statussummary().status() << std::endl;

            // Print Partial failures within the batch if applicable
            if (vrf_msg_resp.statussummary().status() ==
                    service_layer::SLErrorStatus_SLErrno_SL_SOME_ERR) {
                for (int result = 0; result < vrf_msg_resp.results_size(); result++) {
                    auto slerr_status = static_cast<int>(vrf_msg_resp.results(result).errstatus().status());
                    std::cerr << "Error code for vrf " << vrf_msg_resp.results(result).vrfname() << " is 0x" << std::hex << slerr_status << std::endl;
                }
            }
        }
    } else {
        std::cerr << "RPC failed, error code is " << status.error_code() << std::endl;
    }

}

}
