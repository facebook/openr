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
/*#include <iosxrsl/sl_global.grpc.pb.h>
#include <iosxrsl/sl_global.pb.h>
#include <iosxrsl/sl_common_types.pb.h>
#include <iosxrsl/sl_version.pb.h>
*/

#include <xrtelemetry/telemetry.pb.h>
#include <xrtelemetry/telemetry.grpc.pb.h>
#include <xrtelemetry/mdt_grpc_dialin/mdt_grpc_dialin.grpc.pb.h>
#include <xrtelemetry/mdt_grpc_dialin/mdt_grpc_dialin.pb.h>
#include <xrtelemetry/cisco_ios_xr_ipv6_nd_oper/ipv6_node_discovery/nodes/node/neighbor_interfaces/neighbor_interface/host_addresses/host_address/ipv6_nd_neighbor_entry.pb.h>
#include "IosxrTelemetryDecode.h"

namespace openr {

//std::string gpbMsgToJson(const google::protobuf::Message& message);

enum EncodingType {
    IOSXR_TELEMETRY_DIALIN_GPB = 2,
    IOSXR_TELEMETRY_DIALIN_GPBKV = 3
};

struct SubscriptionData 
{
    SubscriptionData(uint64_t reqId,
                     EncodingType encodingType,
                     std::string subscriptionName)
        : req_id(reqId),
          encoding(encodingType),
          subscription(subscriptionName) {}
          
    uint64_t req_id;
    EncodingType encoding;
    std::string subscription;
};

using Credentials = std::unordered_map<std::string, std::string>;

class TelemetryStream {
public:
    explicit TelemetryStream(std::shared_ptr<grpc::Channel> channel);
    ~TelemetryStream();

    void AddSubscription(uint64_t reqId,
                     EncodingType encodingType,
                     std::string subscriptionName)
    {
        subscription_set.emplace_back(
                SubscriptionData(reqId,
                                 encodingType,
                                 subscriptionName));
    }

    void SubscribeAll();    
    void Subscribe(const SubscriptionData& subscription_data);
    void AsyncCompleteRpc();

    void Shutdown();
    void Cleanup();

    void SetCredentials(const std::string& user, 
                        const std::string & passwd) {
        credentials_["username"]=user;
        credentials_["password"]=passwd;
    }

    Credentials  GetCredentials() { return credentials_;}


    std::mutex channel_mutex;
    std::condition_variable channel_condVar;
    bool channel_closed = false;
    // Maintain context of all the  Subscriptions
    std::vector<SubscriptionData> subscription_set;
private:
    // Out of the passed in Channel comes the stub, stored here, our view of the
    // server's exposed services.
    std::unique_ptr<IOSXRExtensibleManagabilityService::gRPCConfigOper::Stub> stub_;

    // The producer-consumer queue we use to communicate asynchronously with the
    // gRPC runtime.
    grpc::CompletionQueue cq_;

    // Dial-In Telemetry connection requires credentials(username/password) to be 
    // passed in as metadata for each RPC. 
    Credentials credentials_;

    // Used as an indicator to exit completion queue thread upon queue shutdown.
    bool tear_down = false;

    class AsyncClientCall {
    private:
        enum CallStatus {CREATE, PROCESS, FINISH};
        CallStatus callStatus_;
    public:
        AsyncClientCall();
        ~AsyncClientCall();

        // Container for the data we expect from the server.
        IOSXRExtensibleManagabilityService::CreateSubsReply createSubsReply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        grpc::ClientContext context;

        // Storage for the status of the RPC upon completion.
        grpc::Status status;

        std::unique_ptr< ::grpc::ClientAsyncReaderInterface< ::IOSXRExtensibleManagabilityService::CreateSubsReply>> response_reader;
        std::unique_ptr<TelemetryDecode> telemetryDecode_;

        void HandleResponse(bool responseStatus, grpc::CompletionQueue* pcq_);      

    };

    std::vector<AsyncClientCall*> callvector_;
};
}
